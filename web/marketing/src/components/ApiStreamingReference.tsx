import { gh } from "../data/apiContent";
import { SITE_BASE } from "../data/content";
import {
  anthropicSseSnippet,
  chatClientConceptSnippet,
  chatResponseUpdateSnippet,
  openaiSseSnippet,
  sessionStreamingSnippet,
  streamPrimitiveSnippet,
  streamingDrainLoopSnippet,
  streamingEntries,
  streamingJokerConformerSnippet,
  workedExampleSnippet,
} from "../data/streamingContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";
import type { Lang } from "../i18n/LanguageContext";

function entriesById(lang: Lang, ids: string[]) {
  return ids.map((id) => {
    const e = streamingEntries[lang].find((entry) => entry.id === id);
    if (!e) throw new Error(`missing streaming entry: ${id}`);
    return e;
  });
}

function DocEntries({ ids }: { ids: string[] }) {
  const { lang } = useLang();
  const tu = ui[lang];
  return (
    <div className="doc-entries">
      {entriesById(lang, ids).map((e) => (
        <article className="doc-entry" id={e.id} key={e.id}>
          <div className="doc-entry-head">
            <code className="api-tag">{e.tag}</code>
            <span className={`status-badge status-${e.status}`}>
              {e.status === "real" ? tu.statusRealTested : tu.statusDesignedNotBuilt}
            </span>
          </div>
          <h3>{e.title}</h3>
          <p>{e.body}</p>
          <a className="api-cite" href={e.href} target="_blank" rel="noreferrer">
            {e.cite}
          </a>
        </article>
      ))}
    </div>
  );
}

function Cite({ path, label }: { path: string; label?: string }) {
  return (
    <a
      className="api-cite"
      href={gh(path)}
      target="_blank"
      rel="noreferrer"
      style={{ borderTop: "none", paddingTop: 0, marginTop: 14, display: "block" }}
    >
      {label ?? path}
    </a>
  );
}

const copy = {
  en: {
    eyebrow: "013 §1 — UI and Streaming Surfaces · 004 §1",
    headingPrefix: "Streaming is",
    headingHighlight: "the required shape, not an add-on",
    intro: (
      <>
        AgentEngine's streaming seam lives on <code>ChatClient</code> itself, not bolted onto{" "}
        <code>AgentSession</code> as a separate ask. Since ADR-035 Phase 3, every conformer must
        implement <code>chat_stream()</code> — the whole-response <code>chat()</code> is now the
        optional one. This page covers the primitive that return type actually is (
        <code>agentengine::stream&lt;T&gt;</code>), what rides inside each update, how a whole{" "}
        <code>AgentSession</code> opts into streaming its turn loop, and — briefly — what is
        actually on the wire underneath a live provider.
      </>
    ),

    s1Eyebrow: "core/chat_client.hpp — ADR-035 Phase 3",
    s1Heading: "chat_stream() is the required method now",
    s1Body: (
      <>
        The <code>ChatClient</code> concept's required shape is exactly two expressions:{" "}
        <code>capabilities()</code> and <code>chat_stream(request, ctx)</code>. That is a real
        framing shift, not a documentation nuance — a backend that implements only{" "}
        <code>chat_stream()</code>, with no <code>chat()</code> at all, now conforms where it
        previously would not have. Nothing that exists today lost anything: every real backend
        still has a <code>chat()</code> and keeps it, and the relaxation was verified
        zero-behavior-change across a 46-file conversion pass before it landed.
      </>
    ),
    s1Note: (
      <>
        <strong>One capability bit this relaxation makes moot.</strong>{" "}
        <code>ChatClientCapabilities::streaming</code> is declared but read by nothing in the
        tree — <code>chat_stream()</code> is always available on any conformer, so there is no
        degradation path that needs to ask permission first. The field round-trips fine; it just
        has no consumer, the same honest gap <a href={`${SITE_BASE}/api/providers.html`}>the
        providers page's capability table</a> already names for several other bits.
      </>
    ),

    s2Eyebrow: "core/stream.hpp:147-228",
    s2Heading: "agentengine::stream<T>: a move-only drain handle over a real channel",
    s2Body: (
      <>
        <code>stream&lt;T&gt;</code> is 004 §1's literal <code>chat_stream()</code> return type. It
        wraps <code>rt::channel_consumer&lt;T, error&gt;</code>: <code>next()</code> returns{" "}
        <code>std::optional&lt;T&gt;</code>, where <code>nullopt</code> means "nothing buffered
        right now, not done" — a caller drains to empty, then checks <code>done()</code>. There is
        no cross-actor OPEN handshake and no actor addressing: <code>chat_stream()</code> is a
        plain, synchronous, in-process call that hands back an already-connected pair.{" "}
        <code>make_stream&lt;T&gt;</code> builds that pair over a bounded channel (default capacity
        256) with one shared <code>std::stop_source</code> — dropping or cancelling the consumer
        fires an out-of-band <code>std::stop_token</code> (ADR-017) that a background read loop can
        observe even while parked in I/O with nothing left to push.
      </>
    ),
    s2CanonicalLabel: "The canonical drain loop",
    s2CanonicalBody:
      "examples/07_streaming.cpp's whole main() — short and self-contained, and the same shape every drain loop in this codebase follows.",

    s3Eyebrow: "core/chat_client.hpp:150-167",
    s3Heading: "ChatResponseUpdate: delta, is_final, and usage that only lands at the end",
    s3Body: (
      <>
        Each pushed item carries a <code>ContentItem delta</code> — the same type a whole{" "}
        <code>ChatResponse</code>'s content is built from — plus <code>is_final</code> and an{" "}
        <code>Usage</code> that is only meaningful once <code>is_final</code> is true.{" "}
        <code>StreamingJokerChatClient</code> below is a real, tested <code>ChatClient</code>{" "}
        conformer that streams for real: one word per push, from a background thread, through a
        ring whose capacity (2) is deliberately smaller than the reply's word count — proving a
        full ring blocks the producer thread rather than dropping an item.
      </>
    ),
    s3Note: (
      <>
        <strong>usage's nullopt is load-bearing.</strong> It means "this backend/call provided
        none," never "assume zero." A caller that needs usage — <code>AgentSession</code>'s
        streaming loop, 004 §5's <code>TokenBudget&lt;N&gt;</code> — must treat a missing usage as
        a hard failure rather than silently costing a streamed call at zero tokens.
      </>
    ),

    s4Eyebrow: "rt/agent_session.hpp:719-720 — ADR-034",
    s4Heading: "Session-level streaming: one flag, the whole turn loop",
    s4Body: (
      <>
        <code>set_stream_model_calls(bool)</code>/<code>stream_model_calls()</code> are plain,
        noexcept accessors over a bool defaulting to <code>false</code> — every existing session is
        unaffected until it opts in. With it set, <code>run_model_call()</code> dispatches to the
        streaming <code>call_stream()</code> path instead of the plain <code>chat()</code> method,
        and each pushed <code>ChatResponseUpdate</code> is projected onto the session's own event
        stream as a <code>run_event_kind::model_delta</code> event. A streamed run also emits one{" "}
        <code>run_event_kind::warning</code> right after <code>run_started</code>, because engaging
        the flag is itself an operator-visible choice — a real, distinct event kind, not a
        fabricated placeholder.
      </>
    ),
    s4Link: "Watching a run's whole progress, not just its text →",
    s4Note: (
      <>
        <strong>What this page does not re-explain.</strong> The event side of{" "}
        <code>examples/29_agent_session_events.cpp</code> — the full{" "}
        <code>run_started</code>/<code>turn_started</code>/<code>model_call_started</code>/
        <code>model_delta</code>/<code>model_call_finished</code>/<code>turn_finished</code>/
        <code>run_finished</code> sequence, and the <code>RunEvent::seq</code> ordering — is the{" "}
        <a href={`${SITE_BASE}/api/events.html#events`}>Events page</a>'s whole subject. This page
        stops at the flag that turns model_delta events on.
      </>
    ),
    s4BundleNote: (
      <>
        <strong>The ergonomic shortcut.</strong>{" "}
        <a href={`${SITE_BASE}/api/builder.html`}>
          <code>QuickstartSessionBuilder</code>
        </a>
        's <code>Bundle::ask_stream()</code> wraps this exact flag for the common case: it calls{" "}
        <code>set_stream_model_calls(true)</code> once, relays <code>model_delta</code> text
        fragments onto a plain <code>stream&lt;std::string&gt;</code>, and closes it on{" "}
        <code>run_finished</code> — streamed text without touching <code>AgentSession</code>{" "}
        directly.
      </>
    ),
    s4WorkflowNote: (
      <>
        <strong>When the backend behind <code>chat_stream()</code> is a whole Workflow, not a
        model.</strong> <code>WorkflowChatClient</code> (GitHub issue #35, ADR-162/163) satisfies
        this same <code>ChatClient</code> concept by wrapping a <code>WorkflowSupervisor</code> —
        but it honestly reports <code>capabilities().streaming == false</code>: it never emits
        token-level deltas from inside the graph, only the wrapped workflow's terminal result (or,
        on a suspended <code>request_port</code>, a <code>Custom</code>-typed human-in-the-loop
        ask). See{" "}
        <a href={`${SITE_BASE}/api/workflow.html#workflow-hitl-chat-client`}>
          Workflow &amp; Orchestration — HITL over chat_client
        </a>{" "}
        for the real wire shape and why it's <code>Custom</code>, never <code>ToolCall</code>.
      </>
    ),

    s5Eyebrow: "protocol/openai · protocol/anthropic — if you're curious",
    s5Heading: "Two SSE wire framings, briefly",
    s5Body: (
      <>
        Everything above is the same regardless of backend. Underneath a live HTTP provider,
        though, the two vendors frame their Server-Sent Events differently, and both feed the same
        shared primitives — <code>ChunkedBodyDecoder</code>, <code>SseEventFramer</code>, one{" "}
        <code>StreamingUpdateAccumulator</code> shape — so the two wire formats cannot drift into
        two separate decoders. Since ADR-019 the bytes are decoded and pushed as they arrive:{" "}
        <code>perform_provider_streaming_exchange</code> delivers fragments to an{" "}
        <code>on_body</code> callback, and each ready update goes straight onto the
        credit-controlled ring above. The full depth of this — capability degradation, credential
        handling, the request bodies themselves — is{" "}
        <a href={`${SITE_BASE}/api/providers.html#streaming`}>the providers page</a>'s subject; this
        is just what is actually on the wire.
      </>
    ),
    s5aLabel: "OpenAI-compatible: data:-only lines",
    s5bLabel: "Anthropic: named events",

    s6Eyebrow: "examples/29_agent_session_events.cpp",
    s6Heading: "Worked example: streaming through a whole session, not just a raw ChatClient",
    s6Body: (
      <>
        This is the same file the session-streaming section above draws from, shown end to end. A
        scripted <code>ChatClient</code> pushes three word-deltas synchronously (no background
        thread — the point here is the event stream, not cross-thread backpressure), and the
        session projects each one onto its own event stream while still returning the same,
        ordinary <code>AgentResponse</code> a non-streaming call would. The two never disagree:
        joining every <code>model_delta</code>'s text in emission order reconstructs exactly the
        final message's text.
      </>
    ),
    s6Note: (
      <>
        <strong>One stale comment worth naming honestly.</strong>{" "}
        <code>examples/07_streaming.cpp</code>'s own header comment says "there is no session-level
        'stream a run' ask yet" — true when that example was written, and no longer true since
        ADR-034 landed <code>set_stream_model_calls()</code> and this example. The primitive{" "}
        07_streaming.cpp demonstrates is still exactly correct; only that one sentence about what
        sits above it is outdated, and this page reports the current state rather than repeating
        it.
      </>
    ),
  },

  vi: {
    eyebrow: "013 §1 — Bề mặt UI và Streaming · 004 §1",
    headingPrefix: "Streaming là",
    headingHighlight: "hình dạng bắt buộc, không phải một tính năng thêm",
    intro: (
      <>
        Ranh giới streaming của AgentEngine nằm ngay trên <code>ChatClient</code>, không phải được
        gắn thêm vào <code>AgentSession</code> như một yêu cầu riêng. Kể từ ADR-035 Phase 3, mọi
        bên tuân theo phải cài đặt <code>chat_stream()</code> — <code>chat()</code> trả về nguyên
        văn cả phản hồi giờ là phần tùy chọn. Trang này trình bày kiểu trả về thực sự đó là gì (
        <code>agentengine::stream&lt;T&gt;</code>), bên trong mỗi update mang theo gì, một{" "}
        <code>AgentSession</code> chọn tham gia streaming cả vòng lặp lượt của nó bằng cách nào, và
        — ngắn gọn — thực sự có gì trên dây bên dưới một provider đang chạy thật.
      </>
    ),

    s1Eyebrow: "core/chat_client.hpp — ADR-035 Phase 3",
    s1Heading: "chat_stream() giờ là phương thức bắt buộc",
    s1Body: (
      <>
        Hình dạng bắt buộc của concept <code>ChatClient</code> đúng là hai biểu thức:{" "}
        <code>capabilities()</code> và <code>chat_stream(request, ctx)</code>. Đó là một sự thay
        đổi khung nhìn thật sự, không phải một sắc thái tài liệu — một backend chỉ cài đặt{" "}
        <code>chat_stream()</code>, hoàn toàn không có <code>chat()</code>, giờ tuân theo được
        trong khi trước đây thì không. Không có gì đang tồn tại bị mất đi: mọi backend thật vẫn có{" "}
        <code>chat()</code> và vẫn giữ nó, và việc nới lỏng này đã được xác nhận không đổi hành vi
        qua một đợt chuyển đổi 46 file trước khi triển khai.
      </>
    ),
    s1Note: (
      <>
        <strong>Một bit capability mà việc nới lỏng này khiến trở nên vô nghĩa.</strong>{" "}
        <code>ChatClientCapabilities::streaming</code> được khai báo nhưng không nơi nào trong cây
        mã đọc nó — <code>chat_stream()</code> luôn sẵn có trên mọi bên tuân theo, nên không có
        đường suy giảm nào cần xin phép trước. Trường này vẫn đi trọn vòng bình thường; nó chỉ
        không có bên tiêu thụ, đúng khoảng trống trung thực mà{" "}
        <a href={`${SITE_BASE}/api/providers.html`}>bảng capability của trang providers</a> đã nêu
        tên cho vài bit khác.
      </>
    ),

    s2Eyebrow: "core/stream.hpp:147-228",
    s2Heading: "agentengine::stream<T>: một handle rút cạn chỉ-di-chuyển trên một channel thật",
    s2Body: (
      <>
        <code>stream&lt;T&gt;</code> là kiểu trả về nguyên văn của <code>chat_stream()</code> theo
        004 §1. Nó bọc <code>rt::channel_consumer&lt;T, error&gt;</code>: <code>next()</code> trả
        về <code>std::optional&lt;T&gt;</code>, trong đó <code>nullopt</code> nghĩa là "hiện chưa
        có gì trong bộ đệm, chưa xong" — bên gọi rút cạn tới hết rồi mới kiểm tra{" "}
        <code>done()</code>. Không có bắt tay OPEN xuyên actor và không định địa chỉ actor:{" "}
        <code>chat_stream()</code> là một lệnh gọi đồng bộ, trong-tiến-trình thuần túy, trả về ngay
        một cặp đã được nối sẵn. <code>make_stream&lt;T&gt;</code> dựng cặp đó trên một channel có
        giới hạn (dung lượng mặc định 256) với một <code>std::stop_source</code> dùng chung —
        buông hoặc hủy phía consumer sẽ phát ra một <code>std::stop_token</code> ngoài băng
        (ADR-017) mà một vòng đọc nền có thể quan sát ngay cả khi đang mắc kẹt trong I/O và không
        còn gì để đẩy.
      </>
    ),
    s2CanonicalLabel: "Vòng rút cạn chuẩn mực",
    s2CanonicalBody:
      "Toàn bộ main() của examples/07_streaming.cpp — ngắn gọn và tự đủ, đúng hình dạng mà mọi vòng rút cạn trong cây mã này đi theo.",

    s3Eyebrow: "core/chat_client.hpp:150-167",
    s3Heading: "ChatResponseUpdate: delta, is_final, và usage chỉ đến ở lần cuối",
    s3Body: (
      <>
        Mỗi mục được đẩy ra mang một <code>ContentItem delta</code> — cùng kiểu mà nội dung của cả
        một <code>ChatResponse</code> được dựng từ đó — cộng với <code>is_final</code> và một{" "}
        <code>Usage</code> chỉ có ý nghĩa khi <code>is_final</code> là true.{" "}
        <code>StreamingJokerChatClient</code> bên dưới là một bên tuân theo <code>ChatClient</code>{" "}
        thật, có kiểm thử, streaming thật sự: mỗi từ một lần đẩy, từ một luồng nền, qua một vòng
        đệm có dung lượng (2) cố ý nhỏ hơn số từ của câu trả lời — chứng minh một vòng đệm đầy sẽ
        chặn luồng producer chứ không làm rớt một mục nào.
      </>
    ),
    s3Note: (
      <>
        <strong>nullopt của usage mang tính chịu lực.</strong> Nó nghĩa là "backend/lệnh gọi này
        không cung cấp gì cả," không bao giờ là "cứ coi như bằng không." Một bên gọi cần usage —
        vòng lặp streaming của <code>AgentSession</code>, <code>TokenBudget&lt;N&gt;</code> của 004
        §5 — phải coi một usage thiếu là một lỗi cứng, thay vì lặng lẽ tính một lệnh gọi streaming
        với chi phí bằng không token.
      </>
    ),

    s4Eyebrow: "rt/agent_session.hpp:719-720 — ADR-034",
    s4Heading: "Streaming ở cấp session: một cờ, cả vòng lặp lượt",
    s4Body: (
      <>
        <code>set_stream_model_calls(bool)</code>/<code>stream_model_calls()</code> là những
        accessor noexcept đơn giản trên một bool mặc định <code>false</code> — mọi session sẵn có
        không bị ảnh hưởng cho tới khi tự chọn tham gia. Khi được bật,{" "}
        <code>run_model_call()</code> chuyển sang đường <code>call_stream()</code> dạng streaming
        thay vì phương thức <code>chat()</code> thuần túy, và mỗi <code>ChatResponseUpdate</code>{" "}
        được đẩy ra sẽ được chiếu lên luồng sự kiện riêng của session dưới dạng một sự kiện{" "}
        <code>run_event_kind::model_delta</code>. Một lượt chạy dạng streaming cũng phát ra đúng
        một <code>run_event_kind::warning</code> ngay sau <code>run_started</code>, vì việc bật cờ
        này tự nó là một lựa chọn đáng để người vận hành nhìn thấy — một loại sự kiện có thật,
        riêng biệt, không phải một loại bịa ra.
      </>
    ),
    s4Link: "Theo dõi toàn bộ tiến trình một lượt chạy, không chỉ văn bản →",
    s4Note: (
      <>
        <strong>Điều trang này không giải thích lại.</strong> Phần sự kiện của{" "}
        <code>examples/29_agent_session_events.cpp</code> — chuỗi đầy đủ{" "}
        <code>run_started</code>/<code>turn_started</code>/<code>model_call_started</code>/
        <code>model_delta</code>/<code>model_call_finished</code>/<code>turn_finished</code>/
        <code>run_finished</code>, và thứ tự <code>RunEvent::seq</code> — là chủ đề trọn vẹn của{" "}
        <a href={`${SITE_BASE}/api/events.html#events`}>trang Events</a>. Trang này dừng lại ở cái
        cờ bật sự kiện model_delta.
      </>
    ),
    s4BundleNote: (
      <>
        <strong>Lối tắt tiện dụng.</strong>{" "}
        <a href={`${SITE_BASE}/api/builder.html`}>
          <code>QuickstartSessionBuilder</code>
        </a>
        's <code>Bundle::ask_stream()</code> bọc đúng cờ này cho trường hợp phổ biến: nó gọi{" "}
        <code>set_stream_model_calls(true)</code> một lần, chuyển tiếp các mảnh văn bản{" "}
        <code>model_delta</code> lên một <code>stream&lt;std::string&gt;</code> thuần túy, rồi đóng
        nó lại khi <code>run_finished</code> — có văn bản streaming mà không cần chạm trực tiếp vào{" "}
        <code>AgentSession</code>.
      </>
    ),
    s4WorkflowNote: (
      <>
        <strong>Khi backend đứng sau <code>chat_stream()</code> là cả một Workflow, không phải
        một model.</strong> <code>WorkflowChatClient</code> (GitHub issue #35, ADR-162/163) thỏa
        mãn cùng khái niệm <code>ChatClient</code> này bằng cách bọc một{" "}
        <code>WorkflowSupervisor</code> — nhưng nó báo cáo trung thực{" "}
        <code>capabilities().streaming == false</code>: nó không bao giờ phát ra delta cấp token
        từ bên trong đồ thị, chỉ có kết quả CUỐI CÙNG của workflow được bọc (hoặc, khi một{" "}
        <code>request_port</code> bị đình chỉ, một câu hỏi human-in-the-loop kiểu{" "}
        <code>Custom</code>). Xem{" "}
        <a href={`${SITE_BASE}/api/workflow.html#workflow-hitl-chat-client`}>
          Workflow &amp; Orchestration — HITL qua chat_client
        </a>{" "}
        để biết hình dạng wire thật và vì sao đó là <code>Custom</code>, không bao giờ là{" "}
        <code>ToolCall</code>.
      </>
    ),

    s5Eyebrow: "protocol/openai · protocol/anthropic — nếu bạn tò mò",
    s5Heading: "Hai kiểu đóng khung SSE trên dây, ngắn gọn",
    s5Body: (
      <>
        Mọi thứ ở trên đều giống nhau bất kể backend nào. Nhưng bên dưới một provider HTTP đang
        chạy thật, hai nhà cung cấp đóng khung Server-Sent Events của họ khác nhau, và cả hai đều
        dùng chung các nguyên thủy giống nhau — <code>ChunkedBodyDecoder</code>,{" "}
        <code>SseEventFramer</code>, một hình dạng <code>StreamingUpdateAccumulator</code> — nên
        hai định dạng trên dây không thể trôi thành hai bộ giải mã khác nhau. Kể từ ADR-019, các
        byte được giải mã và đẩy đi ngay khi tới nơi: <code>perform_provider_streaming_exchange</code>{" "}
        giao các mảnh cho một callback <code>on_body</code>, và mỗi update đã sẵn sàng đi thẳng lên
        vòng đệm có kiểm soát tín dụng ở trên. Chiều sâu đầy đủ của phần này — suy giảm capability,
        xử lý credential, chính các thân request — là chủ đề của{" "}
        <a href={`${SITE_BASE}/api/providers.html#streaming`}>trang providers</a>; đây chỉ là những
        gì thực sự có trên dây.
      </>
    ),
    s5aLabel: "Tương thích OpenAI: chỉ các dòng data:",
    s5bLabel: "Anthropic: các sự kiện có tên",

    s6Eyebrow: "examples/29_agent_session_events.cpp",
    s6Heading: "Ví dụ minh họa: streaming qua cả một session, không chỉ một ChatClient thô",
    s6Body: (
      <>
        Đây chính là file mà phần streaming ở cấp session bên trên lấy ra, được trình bày từ đầu
        tới cuối. Một <code>ChatClient</code> có kịch bản đẩy ba delta-từ theo cách đồng bộ (không
        có luồng nền — trọng tâm ở đây là luồng sự kiện, không phải backpressure xuyên luồng), và
        session chiếu từng cái lên luồng sự kiện riêng của nó trong khi vẫn trả về đúng một{" "}
        <code>AgentResponse</code> bình thường như một lệnh gọi không streaming sẽ trả về. Hai bên
        không bao giờ mâu thuẫn: nối văn bản của mọi <code>model_delta</code> theo đúng thứ tự phát
        ra sẽ dựng lại chính xác văn bản của message cuối cùng.
      </>
    ),
    s6Note: (
      <>
        <strong>Một dòng chú thích đã cũ, đáng nói thẳng.</strong> Chú thích đầu file của{" "}
        <code>examples/07_streaming.cpp</code> viết "chưa có yêu cầu 'stream một lượt chạy' ở cấp
        session" — đúng vào lúc ví dụ đó được viết, và không còn đúng nữa kể từ khi ADR-034 đưa{" "}
        <code>set_stream_model_calls()</code> và ví dụ này vào. Nguyên lý mà 07_streaming.cpp trình
        bày vẫn hoàn toàn chính xác; chỉ riêng câu đó về thứ nằm phía trên nó là đã lỗi thời, và
        trang này thuật lại trạng thái hiện tại thay vì lặp lại câu đó.
      </>
    ),
  },
} as const;

export function ApiStreamingReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];

  return (
    <section className="section" id="streaming-reference">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 780 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {tu.statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        {/* ---- 1. chat_stream() is required ----------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="seam" style={{ marginTop: 48, marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="include/agentengine/core/chat_client.hpp">
              {highlightCpp(chatClientConceptSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <DocEntries ids={["entry-chat-client-concept"]} />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s1Note}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. stream<T> primitive ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div
              className="section-head anchor-target"
              id="stream-primitive"
              style={{ marginTop: 56, marginBottom: 22 }}
            >
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="include/agentengine/core/stream.hpp">
              {highlightCpp(streamPrimitiveSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <DocEntries ids={["entry-stream-type", "entry-make-stream"]} />
          </RevealItem>

          <RevealItem>
            <div className="gs-recommend">
              <span className="gs-recommend-label">{t.s2CanonicalLabel}</span>
              <p>{t.s2CanonicalBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/07_streaming.cpp">
              {highlightCpp(streamingDrainLoopSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. ChatResponseUpdate --------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div
              className="section-head anchor-target"
              id="response-update"
              style={{ marginTop: 56, marginBottom: 22 }}
            >
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="include/agentengine/core/chat_client.hpp">
              {highlightCpp(chatResponseUpdateSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <DocEntries ids={["entry-response-update"]} />
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/07_streaming.cpp">
              {highlightCpp(streamingJokerConformerSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              {t.s3Note}
            </p>
          </RevealItem>

          <RevealItem>
            <Cite
              path="tests/test_chat_client_stream.cpp"
              label="tests/test_chat_client_stream.cpp — the same primitive, proven end to end"
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Session-level streaming ---------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div
              className="section-head anchor-target"
              id="session-streaming"
              style={{ marginTop: 56, marginBottom: 22 }}
            >
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
              <a
                href={`${SITE_BASE}/api/events.html#events`}
                style={{ display: "inline-block", marginTop: 12, color: "var(--accent-teal)", fontSize: "0.9rem" }}
              >
                {t.s4Link}
              </a>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/29_agent_session_events.cpp">
              {highlightCpp(sessionStreamingSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <DocEntries ids={["entry-session-streaming"]} />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s4Note}</p>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 12, borderLeftColor: "var(--accent-teal)" }}>
              {t.s4BundleNote}
            </p>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 12 }}>{t.s4WorkflowNote}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. Two SSE wire framings -------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="wire" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <p style={{ fontWeight: 600, marginBottom: 8 }}>{t.s5aLabel}</p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="protocol/openai/chat_client.hpp">
              {highlightCpp(openaiSseSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p style={{ fontWeight: 600, margin: "20px 0 8px" }}>{t.s5bLabel}</p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="protocol/anthropic/chat_client.hpp">
              {highlightCpp(anthropicSseSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Worked example ---------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div
              className="section-head anchor-target"
              id="worked-example"
              style={{ marginTop: 56, marginBottom: 22 }}
            >
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/29_agent_session_events.cpp">
              {highlightCpp(workedExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              {t.s6Note}
            </p>
          </RevealItem>

          <RevealItem>
            <Cite
              path="examples/29_agent_session_events.cpp"
              label="examples/29_agent_session_events.cpp — mirrors tests/test_rt_agent_session_streaming_and_events.cpp's S1/A2"
            />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
