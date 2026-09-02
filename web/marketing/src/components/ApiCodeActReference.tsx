import {
  agentAskHitlSnippet,
  agentFilesDataGeneratedSnippet,
  agentFilesDataUsageSnippet,
  agentLibraryRegistrySnippet,
  agentModuleRegistry,
  codeActBridgeConfigSnippet,
  codeActEntries,
  codeActGeneratedFnSnippet,
  codeActUnionSnippet,
  gh,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

function StatusBadge({ status }: { status: "real" | "design" }) {
  const { lang } = useLang();
  const t = ui[lang];
  return (
    <span className={`status-badge status-${status}`}>
      {status === "real" ? t.statusRealTested : t.statusDesignedNotBuilt}
    </span>
  );
}

const copy = {
  en: {
    eyebrow: "026 — Agent-Facing Runtime Surface",
    headingPrefix: "CodeAct is",
    headingHighlight: "execute_code",
    headingSuffix: ", not a second tool",
    introBody: (
      <>
        There is no <code>codeact</code> tool anywhere in this codebase. CodeAct is the{" "}
        <code>execute_code</code> tool — the one <code>using-the-code-interpreter</code> teaches
        — used together with the <code>agent</code> Python library present in the sandbox. The
        library is the action space: instead of the model naming one tool per action, it writes
        ordinary Python against <code>agent.*</code>, and the interpreter executes it under the
        same capability-gated tool pipeline every other call goes through.
      </>
    ),
    introNote: (
      <>
        026 §5 names the nine <code>agent.*</code> modules this page audits.
      </>
    ),
    introUnionBody: (
      <>
        Three of those nine modules are real and reach the pipeline; six exist only as a
        registry entry. This page draws that line module by module.
      </>
    ),
    introToolsBody: (
      <>
        <code>agent.tools</code> exposes the union of the agent's own declared tools, tools
        unlocked by currently mounted skills, and tools discovered from a connected MCP server.
        All three sources are real and tested. The agent's own tools and skill-unlocked tools are
        live-wired in <code>tools/cli_chat.cpp</code> today; the MCP source has no real server in
        this codebase to connect it to yet.
      </>
    ),
    flow1Title: "Model writes ordinary Python",
    flow1Sub: 'agent.tools.word_count("some text") — inside a real execute_code call',
    flow1Arrow: "the generated function calls _ae_internal.call_tool(...)",
    flow2Title: "JSON crosses the C++/Python boundary",
    flow2Sub: "the same capability-gated invoke_tool() pipeline every model-declared tool call goes through",
    flow2Arrow: "json.loads() + wrapped, same interpreter call",
    flow3Sub: 'reply.hits, not reply["hits"] — an object, not raw JSON text',
    loopNote: "No second round trip through the model just to parse or reformat a reply — the whole loop above happens once, inside the sandbox.",
    s1Eyebrow: "trust/agent_library_manifest.hpp — §5's own registry",
    s1Heading: (
      <>
        The nine <code>agent.*</code> modules, one shared source of truth
      </>
    ),
    s1Body: (
      <>
        This registry is the one place both halves read from: the real{" "}
        <code>dir(agent)</code>/<code>help(agent)</code> introspection story, and the
        model-facing prompt summary. They can't drift from each other. A module whose gating
        capability isn't in the caller's <code>CapabilitySet</code> is simply absent from both —
        never listed as present, never explained as denied.
      </>
    ),
    s1Note: <>I2 — no ambient authority.</>,
    moduleTableColumns: ["Module", "Purpose", "Status", "Gated by"],
    registrySnippetIntro: (
      <>
        The table above renders this struct directly. It can't show what makes three of nine
        modules "real," though: a <code>ModuleDescriptor</code> entry existing here is necessary
        but not sufficient. <code>agent.tools</code>, <code>agent.files</code>, and{" "}
        <code>agent.data</code> also have a real <code>*_codegen.hpp</code> generator and a live
        bootstrap call. The other six have only the row below — nothing else in the codebase.
      </>
    ),
    s2Eyebrow: "The two real bridges",
    s2Heading: (
      <>
        <code>agent.tools</code> and <code>agent.files</code>/<code>agent.data</code> — real
        code, proven against a real embedded interpreter
      </>
    ),
    s2bEyebrow: "src/backends/native_jail/agent_files_data_codegen.hpp",
    s2bHeading: "The other real bridge, same generated-source shape",
    s2bBody: (
      <>
        Unlike <code>agent.tools</code>, this header isn't a generator over caller-supplied
        schema. The function set is fixed, so it's a pair of static Python source strings. Every{" "}
        <code>_ae_internal.open()</code>/<code>_ae_internal.listdir()</code> call inside them
        still goes through the same per-call <code>cap::FsRead</code>/<code>cap::FsWrite</code>{" "}
        check the raw primitives already enforce. <code>agent.files</code> and{" "}
        <code>agent.data</code> widen nothing — they're convenience only. The two generators,{" "}
        <code>read_json_lines</code> and <code>read_csv_rows</code>, never materialize a whole
        file; that's what makes the "without loading them wholly into memory" claim literal.
      </>
    ),
    s2bNote: (
      <>
        026 §5 fixes this function set and states the "without loading them wholly into memory"
        guarantee.
      </>
    ),
    s2bUsageEyebrow: "tests/test_mediated_python_runner_agent_files_data.cpp",
    s2bUsageBody: (
      <>
        <code>agent.files.input</code> reads real bytes back. <code>agent.files.artifact</code>{" "}
        writes a real file that lands on the host disk. <code>agent.files.list</code> reports
        real directory entries — all three proven against a real embedded interpreter and a real
        scratch mount directory.
      </>
    ),
    s3Eyebrow: "What a bridged tool looks like from inside CodeAct",
    s3Heading: "One generated function per tool, one shared reply wrapper",
    s3Body: (
      <>
        <code>agent_tools_codegen.hpp</code> builds this source text directly from a real{" "}
        <code>ToolDescriptor</code> at <code>initialize()</code> time. The generated Python never
        re-parses its own schema at runtime. The return value is <code>_AeReply</code>, an
        object: attribute access (<code>reply.hits</code>), not dict indexing (
        <code>reply["hits"]</code>). That's deliberate — <code>_ae_internal.call_tool</code>{" "}
        returns raw JSON text over the wire, and <code>_AeReply</code> wraps it before CodeAct
        code ever sees it.
      </>
    ),
    s3Note: (
      <>
        <strong>The JSON round trip happens once, inside the sandbox, never back through the
        model.</strong> <code>_ae_internal.call_tool</code> returns a JSON string across the
        C++/Python boundary. The generated function calls <code>json.loads</code> on it and
        wraps the result in <code>_AeReply</code>, in the same interpreter call that made the
        request. CodeAct code reads <code>reply.field_name</code> immediately — there's no
        second round trip through the model just to parse or reformat a tool's reply.
      </>
    ),
    s4Eyebrow: 'The real answer to "can CodeAct call a mounted skill\'s tools?"',
    s4Heading: "Yes — live in the CLI, rebuilt fresh on every execute_code call",
    s4Body: (
      <>
        <code>MediatedPythonRunner::refresh_agent_tools(ToolBridgeConfig)</code> reconfigures{" "}
        <code>agent.tools</code> on an already-initialized interpreter. It re-runs the same
        bootstrap <code>initialize()</code> ran once, against a fresh throwaway globals dict,
        without ever tearing the interpreter down. <code>core/codeact_tool_union.hpp</code>'s{" "}
        <code>union_codeact_tools()</code> merges the three sources into one bridge-ready{" "}
        <code>ToolTable</code>, rejecting a name collision across any two sources rather than
        picking a silent precedence order. <code>tools/cli_chat.cpp</code> calls both, together,
        before every <code>execute_code</code> call — on the same per-turn cadence{" "}
        <code>scope_tools_to_mounted_skills</code> already uses for the model-facing declaration
        side. A skill mounted this same round is reachable from <code>agent.tools</code> on the
        very next call, never a call behind.
      </>
    ),
    s4AdrNote: (
      <>
        ADR-002 §5.5.6 protects "at most one interpreter alive at any instant," not "the
        bootstrap runs only once."
      </>
    ),
    s4EdgeNote: (
      <>
        <strong>The sharp edge this surfaced: a fixed import allow-list.</strong> Stage B's
        meta-path finder computes its allow-set once, inside <code>initialize()</code>, from
        the pre/post-bootstrap <code>sys.modules</code> diff. A session constructed with no{" "}
        <code>tool_bridge</code> never imports <code>json</code> during that one-time
        pre-finder-install window, so <code>refresh_agent_tools()</code> calling{" "}
        <code>import json</code> later — with the finder already installed — was denied:{" "}
        <code>ModuleNotFoundError</code>. The fix: have <code>refresh_agent_tools()</code>{" "}
        extend the keep-set itself, applying the same "<code>json</code> becomes importable
        exactly when <code>agent.tools</code> exists" intent this file's own comments already
        state for the construction-time case — just applied when that decision is made later.
      </>
    ),
    s4EdgeCiteNote: (
      <>
        Regression-proven in <code>test_mediated_python_runner_agent_tools.cpp</code>'s Scenario
        4: refresh from no bridge, to tool A, to a different tool B — A genuinely gone, not an
        additive merge.
      </>
    ),
    s4Trailing: (
      <>
        Proven with a real fixture, not just described: <code>tools/cli_chat.cpp</code> ships
        a <code>codeact-demo</code> skill naming a trivial <code>word_count</code> tool in its{" "}
        <code>allowed-tools</code> — deliberately excluded from the top-level model-callable
        set, reachable only as <code>agent.tools.word_count(...)</code> once{" "}
        <code>codeact-demo</code> is mounted. Running the real binary confirms it: freshly
        started, "Tools declared+invocable" lists only <code>mount_skill</code>.{" "}
        <code>word_count</code> is absent until an agent actually mounts the skill that names
        it — exactly as designed.
      </>
    ),
    s5Eyebrow: "agent.ask — the HITL exception",
    s5Heading: "One module, real end to end: a script that stops and asks a human",
    s5Body: (
      <>
        <code>agent.ask(prompt) -&gt; str</code> is not a fixed-response stub. Calling it from
        inside <code>execute_code</code> genuinely suspends that whole call.{" "}
        <code>AgentSession</code> opens a real{" "}
        <code>Interaction&#123;reason == interaction_reason::codeact_ask&#125;</code>, the same
        kind of record a tool-approval gate suspends with, and a host answers it through the
        same <code>resolve_interaction()</code> every approval flow already calls.
      </>
    ),
    s5Note: (
      <>
        ADR-057 Design B: abort-and-replay — the mechanism the real, single-worker runtime
        substrate can actually support. See the{" "}
        <a href="./durability.html#du-interactions">Durability page</a> for the full
        replay-cost disclosure.
      </>
    ),
    statusNote: (
      <>
        <strong>Status: four of nine modules are real and live; five don't exist in
        code.</strong> <code>agent.tools</code> is real, proven against a real embedded
        interpreter, and wired live in <code>tools/cli_chat.cpp</code>: reconfigured before
        every <code>execute_code</code> call from the union of the agent's own tools and
        mount-unlocked skill tools. <code>agent.files</code> and <code>agent.data</code> are
        real and were already wired live in the CLI. <code>agent.ask</code> is real too, proven
        in the worked example above — a genuine exception to the pattern below, not a fourth
        instance of it. The third union source for <code>agent.tools</code>, MCP-discovered
        tools, is real and tested against a real <code>McpServer</code>/<code>McpClient</code>{" "}
        pair, but has no live server in this codebase to connect it to yet; see the Protocol
        surfaces page. <code>agent.memory</code>, <code>agent.notes</code>,{" "}
        <code>agent.output</code>, <code>agent.progress</code>, and <code>agent.spawn</code>{" "}
        exist only as a name/one-liner/gating-capability triple in{" "}
        <code>agent_library_manifest.hpp</code> — no codegen file, no bootstrap, no bridge.
      </>
    ),
  },
  vi: {
    eyebrow: "026 — Agent-Facing Runtime Surface",
    headingPrefix: "CodeAct chính là",
    headingHighlight: "execute_code",
    headingSuffix: ", không phải một tool thứ hai",
    introBody: (
      <>
        Không có tool <code>codeact</code> nào trong codebase này cả. CodeAct chính là tool{" "}
        <code>execute_code</code> — đúng tool mà <code>using-the-code-interpreter</code> dạy —
        được dùng cùng với thư viện Python <code>agent</code> có mặt trong sandbox. Thư viện này
        là không gian hành động: thay vì model nêu tên một tool cho mỗi hành động, nó viết Python
        bình thường nhắm vào <code>agent.*</code>, và trình thông dịch thực thi nó dưới cùng
        pipeline tool bị kiểm soát bởi capability mà mọi lệnh gọi khác đi qua.
      </>
    ),
    introNote: (
      <>
        026 §5 nêu tên chín module <code>agent.*</code> mà trang này kiểm chứng.
      </>
    ),
    introUnionBody: (
      <>
        Ba trong chín module đó là có thật và chạm tới pipeline; sáu module chỉ tồn tại như một
        mục registry. Trang này vạch rõ ranh giới đó theo từng module.
      </>
    ),
    introToolsBody: (
      <>
        <code>agent.tools</code> phơi bày hợp của các tool agent tự khai báo, tool được mở khóa
        bởi các skill đang mount, và tool được khám phá từ một MCP server đã kết nối. Cả ba
        nguồn đều có thật và đã kiểm thử. Tool của chính agent và tool được skill mở khóa được
        đấu nối trực tiếp trong <code>tools/cli_chat.cpp</code> ngày hôm nay; nguồn MCP hiện
        chưa có server thật nào trong codebase này để kết nối tới.
      </>
    ),
    flow1Title: "Model viết Python bình thường",
    flow1Sub: 'agent.tools.word_count("some text") — bên trong một lệnh gọi execute_code thật',
    flow1Arrow: "hàm được sinh ra gọi _ae_internal.call_tool(...)",
    flow2Title: "JSON băng qua ranh giới C++/Python",
    flow2Sub: "chính pipeline invoke_tool() bị kiểm soát bởi capability mà mọi lệnh gọi tool do model khai báo đi qua",
    flow2Arrow: "json.loads() + bọc lại, cùng một lệnh gọi trình thông dịch",
    flow3Sub: 'reply.hits, không phải reply["hits"] — một đối tượng, không phải văn bản JSON thô',
    loopNote: "Không có vòng quay thứ hai qua model chỉ để phân tích hay định dạng lại một phản hồi — toàn bộ vòng lặp ở trên xảy ra một lần, bên trong sandbox.",
    s1Eyebrow: "trust/agent_library_manifest.hpp — registry riêng của §5",
    s1Heading: (
      <>
        Chín module <code>agent.*</code>, một nguồn sự thật dùng chung
      </>
    ),
    s1Body: (
      <>
        Registry này là nơi duy nhất mà cả hai nửa đều đọc từ đó: câu chuyện introspection{" "}
        <code>dir(agent)</code>/<code>help(agent)</code> thật, và bản tóm tắt prompt hướng tới
        model. Chúng không thể lệch nhau. Một module có capability kiểm soát không nằm trong{" "}
        <code>CapabilitySet</code> của caller thì đơn giản là vắng mặt ở cả hai nơi — không bao
        giờ được liệt kê là có mặt, cũng không bao giờ được giải thích là bị từ chối.
      </>
    ),
    s1Note: <>I2 — không có quyền hạn mặc nhiên.</>,
    moduleTableColumns: ["Module", "Mục đích", "Trạng thái", "Kiểm soát bởi"],
    registrySnippetIntro: (
      <>
        Bảng ở trên hiển thị trực tiếp struct này. Nhưng nó không cho thấy điều gì làm cho ba
        trong chín module là "thật": việc một mục <code>ModuleDescriptor</code> tồn tại ở đây là
        điều kiện cần, không phải đủ. <code>agent.tools</code>, <code>agent.files</code>, và{" "}
        <code>agent.data</code> còn có một generator <code>*_codegen.hpp</code> thật và một lệnh
        gọi bootstrap sống thật. Sáu module còn lại chỉ có đúng hàng này — không có gì khác
        trong codebase.
      </>
    ),
    s2Eyebrow: "Hai bridge có thật",
    s2Heading: (
      <>
        <code>agent.tools</code> và <code>agent.files</code>/<code>agent.data</code> — mã có
        thật, đã chứng minh trên một trình thông dịch nhúng thật
      </>
    ),
    s2bEyebrow: "src/backends/native_jail/agent_files_data_codegen.hpp",
    s2bHeading: "Bridge có thật thứ hai, cùng hình dạng mã nguồn được sinh ra",
    s2bBody: (
      <>
        Khác với <code>agent.tools</code>, header này không phải một generator dựa trên schema do
        caller cung cấp. Tập hàm này là cố định, nên đây là một cặp chuỗi mã nguồn Python tĩnh.
        Mỗi lệnh gọi <code>_ae_internal.open()</code>/<code>_ae_internal.listdir()</code> bên
        trong chúng vẫn đi qua đúng kiểm tra <code>cap::FsRead</code>/<code>cap::FsWrite</code>{" "}
        theo từng lệnh gọi mà các primitive thô đã thực thi. <code>agent.files</code> và{" "}
        <code>agent.data</code> không mở rộng bất cứ điều gì — chỉ là tiện ích. Hai generator,{" "}
        <code>read_json_lines</code> và <code>read_csv_rows</code>, không bao giờ nạp toàn bộ
        một file vào bộ nhớ; đó là điều khiến tuyên bố "không nạp toàn bộ vào bộ nhớ" trở thành
        nghĩa đen.
      </>
    ),
    s2bNote: (
      <>
        026 §5 cố định tập hàm này và là nguồn của cam kết "không nạp toàn bộ vào bộ nhớ".
      </>
    ),
    s2bUsageEyebrow: "tests/test_mediated_python_runner_agent_files_data.cpp",
    s2bUsageBody: (
      <>
        <code>agent.files.input</code> đọc lại đúng byte thật. <code>agent.files.artifact</code>{" "}
        ghi một file thật xuống đĩa host. <code>agent.files.list</code> báo cáo đúng các entry
        thư mục thật — cả ba đều đã chứng minh trên một trình thông dịch nhúng thật và một thư
        mục mount scratch thật.
      </>
    ),
    s3Eyebrow: "Một tool qua bridge trông ra sao từ bên trong CodeAct",
    s3Heading: "Một hàm được sinh cho mỗi tool, một wrapper phản hồi dùng chung",
    s3Body: (
      <>
        <code>agent_tools_codegen.hpp</code> xây văn bản mã nguồn này trực tiếp từ một{" "}
        <code>ToolDescriptor</code> thật tại thời điểm <code>initialize()</code>. Mã Python
        được sinh ra không bao giờ phân tích lại schema của chính nó lúc chạy. Giá trị trả về
        là <code>_AeReply</code>, một đối tượng: truy cập qua thuộc tính (<code>reply.hits</code>
        ), không phải chỉ số dict (<code>reply["hits"]</code>). Đây là một lựa chọn có chủ đích
        — <code>_ae_internal.call_tool</code> trả về văn bản JSON thô qua dây, và{" "}
        <code>_AeReply</code> bọc nó lại trước khi mã CodeAct nhìn thấy.
      </>
    ),
    s3Note: (
      <>
        <strong>Vòng quay JSON chỉ xảy ra một lần, bên trong sandbox, không bao giờ quay lại
        qua model.</strong> <code>_ae_internal.call_tool</code> trả về một chuỗi JSON băng
        qua ranh giới C++/Python. Hàm được sinh ra gọi <code>json.loads</code> trên đó và bọc
        kết quả trong <code>_AeReply</code>, trong cùng một lệnh gọi trình thông dịch đã tạo ra
        request. Mã CodeAct đọc <code>reply.field_name</code> ngay lập tức — không có vòng
        quay thứ hai qua model chỉ để phân tích hay định dạng lại phản hồi của một tool.
      </>
    ),
    s4Eyebrow: 'Câu trả lời thật cho "CodeAct có gọi được tool của một skill đã mount không?"',
    s4Heading: "Có — hoạt động trực tiếp trong CLI, được xây lại mới ở mỗi lệnh gọi execute_code",
    s4Body: (
      <>
        <code>MediatedPythonRunner::refresh_agent_tools(ToolBridgeConfig)</code> cấu hình lại{" "}
        <code>agent.tools</code> trên một trình thông dịch đã được khởi tạo từ trước. Nó chạy
        lại đúng bootstrap mà <code>initialize()</code> đã chạy một lần, nhắm vào một globals
        dict dùng-một-lần mới, không bao giờ phá hủy trình thông dịch.{" "}
        <code>union_codeact_tools()</code> của <code>core/codeact_tool_union.hpp</code> gộp ba
        nguồn thành một <code>ToolTable</code> sẵn sàng cho bridge, từ chối một xung đột tên
        giữa bất kỳ hai nguồn nào thay vì chọn một thứ tự ưu tiên âm thầm.{" "}
        <code>tools/cli_chat.cpp</code> gọi cả hai, cùng nhau, trước mỗi lệnh gọi{" "}
        <code>execute_code</code> — theo đúng nhịp mỗi lượt mà{" "}
        <code>scope_tools_to_mounted_skills</code> đã dùng cho phía khai báo hướng tới model.
        Một skill được mount cùng vòng này có thể chạm tới được từ <code>agent.tools</code> ngay
        ở lệnh gọi tiếp theo, không bao giờ chậm một lệnh gọi.
      </>
    ),
    s4AdrNote: (
      <>
        ADR-002 §5.5.6 bảo vệ "nhiều nhất một trình thông dịch sống tại bất kỳ thời điểm nào",
        không phải "bootstrap chỉ chạy một lần".
      </>
    ),
    s4EdgeNote: (
      <>
        <strong>Điểm gai góc mà điều này phơi bày ra: một allow-list import cố định.</strong>{" "}
        Meta-path finder của Stage B tính tập allow của nó một lần, bên trong{" "}
        <code>initialize()</code>, từ sự khác biệt <code>sys.modules</code> trước/sau
        bootstrap. Một session được khởi tạo không có <code>tool_bridge</code> không bao giờ
        import <code>json</code> trong cửa sổ một-lần trước-khi-cài-finder đó, nên{" "}
        <code>refresh_agent_tools()</code> gọi <code>import json</code> sau đó — khi finder
        đã được cài — bị từ chối: <code>ModuleNotFoundError</code>. Cách sửa: để{" "}
        <code>refresh_agent_tools()</code> tự mở rộng tập giữ lại, áp dụng cùng ý định "
        <code>json</code> trở nên import được đúng lúc <code>agent.tools</code> tồn tại" mà
        chính comment của file này đã nêu cho trường hợp tại thời điểm khởi tạo — chỉ là áp
        dụng khi quyết định đó được đưa ra muộn hơn.
      </>
    ),
    s4EdgeCiteNote: (
      <>
        Đã được chứng minh chống hồi quy trong Kịch bản 4 của{" "}
        <code>test_mediated_python_runner_agent_tools.cpp</code>: refresh từ không có bridge,
        sang tool A, sang một tool B khác — A thực sự biến mất, không phải một phép gộp cộng
        thêm.
      </>
    ),
    s4Trailing: (
      <>
        Đã được chứng minh bằng một fixture thật, không chỉ được mô tả:{" "}
        <code>tools/cli_chat.cpp</code> phát hành một skill <code>codeact-demo</code> nêu tên
        một tool <code>word_count</code> đơn giản trong <code>allowed-tools</code> của nó —
        cố ý loại khỏi tập gọi-được-bởi-model ở cấp cao nhất, chỉ chạm tới được dưới dạng{" "}
        <code>agent.tools.word_count(...)</code> một khi <code>codeact-demo</code> được
        mount. Chạy binary thật xác nhận điều đó: mới khởi động, "Tools declared+invocable"
        chỉ liệt kê <code>mount_skill</code>. <code>word_count</code> vắng mặt cho đến khi
        một agent thực sự mount skill nêu tên nó — đúng như thiết kế.
      </>
    ),
    s5Eyebrow: "agent.ask — trường hợp ngoại lệ HITL",
    s5Heading: "Một module, có thật từ đầu đến cuối: một script dừng lại và hỏi con người",
    s5Body: (
      <>
        <code>agent.ask(prompt) -&gt; str</code> không phải một stub trả lời cố định. Gọi nó
        từ bên trong <code>execute_code</code> thực sự treo lại toàn bộ lệnh gọi đó.{" "}
        <code>AgentSession</code> mở một{" "}
        <code>Interaction&#123;reason == interaction_reason::codeact_ask&#125;</code> có thật,
        cùng loại bản ghi mà một cổng phê duyệt tool treo lại, và một host trả lời nó qua đúng{" "}
        <code>resolve_interaction()</code> mà mọi luồng phê duyệt khác đã gọi.
      </>
    ),
    s5Note: (
      <>
        ADR-057 Design B: abort-and-replay — cơ chế mà nền tảng runtime single-worker thật sự
        có thể hỗ trợ. Xem{" "}
        <a href="./durability.html#du-interactions">trang Durability</a> để biết đầy đủ cái
        giá của việc phát lại.
      </>
    ),
    statusNote: (
      <>
        <strong>Trạng thái: bốn trong chín module là thật và hoạt động; năm module không tồn
        tại trong mã.</strong> <code>agent.tools</code> là thật, đã chứng minh trên một trình
        thông dịch nhúng thật, và được đấu nối trực tiếp trong <code>tools/cli_chat.cpp</code>:
        được cấu hình lại trước mỗi lệnh gọi <code>execute_code</code> từ hợp của tool của
        chính agent và tool được skill mở khóa. <code>agent.files</code> và{" "}
        <code>agent.data</code> là thật và đã được đấu nối trực tiếp trong CLI từ trước.{" "}
        <code>agent.ask</code> cũng là thật, đã chứng minh ở ví dụ minh họa bên trên — một
        ngoại lệ thật sự so với khuôn mẫu bên dưới, không phải một trường hợp thứ tư của nó.
        Nguồn hợp thứ ba cho <code>agent.tools</code>, tool khám phá qua MCP, là thật và đã
        kiểm thử trên một cặp <code>McpServer</code>/<code>McpClient</code> thật, nhưng chưa
        có server sống nào trong codebase này để kết nối tới; xem trang Bề mặt giao thức.{" "}
        <code>agent.memory</code>, <code>agent.notes</code>, <code>agent.output</code>,{" "}
        <code>agent.progress</code>, và <code>agent.spawn</code> chỉ tồn tại như một bộ ba
        tên/mô tả-một-dòng/capability-kiểm-soát trong <code>agent_library_manifest.hpp</code> —
        không có file codegen, không bootstrap, không bridge.
      </>
    ),
  },
} as const;

export function ApiCodeActReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" id="codeact">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
            {t.headingSuffix}
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {tu.statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.introBody}</p>
          <ApiDiagnosticNote>{t.introNote}</ApiDiagnosticNote>
          <p style={{ marginTop: 12 }}>{t.introUnionBody}</p>
          <p style={{ marginTop: 12 }}>{t.introToolsBody}</p>
        </div>

        <RevealGroup>
          <RevealItem>
            <div className="flow glass">
              <div className="flow-node is-purple">
                <div className="flow-node-title">{t.flow1Title}</div>
                <div className="flow-node-sub">{t.flow1Sub}</div>
              </div>
              <div className="flow-arrow">{t.flow1Arrow}</div>
              <div className="flow-node">
                <div className="flow-node-title">{t.flow2Title}</div>
                <div className="flow-node-sub">{t.flow2Sub}</div>
              </div>
              <div className="flow-arrow">{t.flow2Arrow}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">_AeReply</div>
                <div className="flow-node-sub">{t.flow3Sub}</div>
              </div>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="flow-loop-note" style={{ marginTop: 14, marginBottom: 48 }}>{t.loopNote}</div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginBottom: 22 }} id="codeact-modules">
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
              <ApiDiagnosticNote>{t.s1Note}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.moduleTableColumns]}
              templateColumns="0.8fr 2.1fr 1.1fr 1.3fr"
              rows={agentModuleRegistry[lang].map((m) => [
                <code key="name">agent.{m.name}</code>,
                m.oneLine,
                <StatusBadge key="status" status={m.status} />,
                <code key="gate">{m.gatedBy}</code>,
              ])}
            />
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24 }}>{t.registrySnippetIntro}</p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="trust/agent_library_manifest.hpp">
              {highlightCpp(agentLibraryRegistrySnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="codeact-bridges">
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="doc-entries">
              {codeActEntries[lang].map((e) => (
                <article className="doc-entry" id={e.id} key={e.id}>
                  <div className="doc-entry-head">
                    <code className="api-tag">{e.tag}</code>
                    <StatusBadge status={e.status} />
                  </div>
                  <h3>{e.title}</h3>
                  <p>{e.body}</p>
                  <a className="api-cite" href={e.href} target="_blank" rel="noreferrer">
                    {e.cite}
                  </a>
                </article>
              ))}
            </div>
          </RevealItem>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 40, marginBottom: 16 }} id="codeact-files-data-generated">
              <span className="eyebrow">{t.s2bEyebrow}</span>
              <h3 style={{ fontSize: "1.2rem", margin: "10px 0" }}>{t.s2bHeading}</h3>
              <p>{t.s2bBody}</p>
              <ApiDiagnosticNote>{t.s2bNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="agent.files / agent.data (generated)">
              {highlightCpp(agentFilesDataGeneratedSnippet)}
            </CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24, marginBottom: 0 }}>
              <span className="eyebrow" style={{ display: "block", marginBottom: 8 }}>{t.s2bUsageEyebrow}</span>
              {t.s2bUsageBody}
            </p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="test_mediated_python_runner_agent_files_data.cpp">
              {highlightCpp(agentFilesDataUsageSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="codeact-generated">
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="agent.tools (generated)">{highlightCpp(codeActGeneratedFnSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24 }}>{t.s3Note}</p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="tool_bridge.hpp / mediated_python_runner.hpp">
              {highlightCpp(codeActBridgeConfigSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="codeact-skills">
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
              <ApiDiagnosticNote>{t.s4AdrNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="codeact_tool_union.hpp / cli_chat.cpp">
              {highlightCpp(codeActUnionSnippet)}
            </CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24 }}>{t.s4EdgeNote}</p>
            <ApiDiagnosticNote>{t.s4EdgeCiteNote}</ApiDiagnosticNote>
          </RevealItem>
          <RevealItem>
            <p style={{ marginTop: 14, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.s4Trailing}</p>
          </RevealItem>
          <RevealItem>
            <div style={{ marginTop: 14, display: "flex", gap: 16, flexWrap: "wrap" }}>
              <a href={gh("tools/cli_chat.cpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                tools/cli_chat.cpp
              </a>
              <a href={gh("include/agentengine/core/codeact_tool_union.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                include/agentengine/core/codeact_tool_union.hpp
              </a>
              <a href={gh("include/agentengine/protocol/mcp/mcp_tool_bridge.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                include/agentengine/protocol/mcp/mcp_tool_bridge.hpp
              </a>
              <a href={gh("src/backends/native_jail/mediated_python_runner.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                src/backends/native_jail/mediated_python_runner.hpp
              </a>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="codeact-agent-ask">
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
              <ApiDiagnosticNote>{t.s5Note}</ApiDiagnosticNote>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="agent_ask_codegen.hpp + agent_session.hpp">
              {highlightCpp(agentAskHitlSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div
              className="gs-note anchor-target"
              id="codeact-status"
              style={{ marginTop: 40, borderLeftColor: "var(--accent-pink)" }}
            >
              {t.statusNote}
              <div style={{ marginTop: 8, display: "flex", gap: 16, flexWrap: "wrap" }}>
                <a href={gh("include/agentengine/trust/agent_library_manifest.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/trust/agent_library_manifest.hpp
                </a>
                <a href={gh("026-Agent-Facing-Runtime-Surface.md")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  026-Agent-Facing-Runtime-Surface.md §5
                </a>
                <a href={gh("tests/test_mediated_python_runner_agent_tools.cpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  tests/test_mediated_python_runner_agent_tools.cpp
                </a>
                <a href={gh("tests/test_codeact_tool_union.cpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  tests/test_codeact_tool_union.cpp
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
