import { SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";

/** Shown once per detail page, collapsed by default. Every page in this family renders snippets
 * that are excerpts from real, tested files, and those files carry their own local helpers — which
 * is the single most common reason a reader cannot paste a snippet and compile it. The panel chrome
 * names the source file; this says what that implies, and names the one helper with a real engine
 * equivalent (`drive` / `rt::block_on`). Native <details>, so it costs no JavaScript state. */
const copy = {
  en: {
    kind: "note",
    summary: "How to read the code on this page",
    lead: "Every snippet here is an excerpt from a real, tested file — the panel's title bar names it and links it. That has one consequence worth knowing before you try to compile one: a few names in these snippets are the source file's own helpers, not engine API.",
    rows: [
      [
        "drive(task)",
        "Example-local, not engine API. The engine's real equivalent is agentengine::rt::block_on(task) (rt/block_on.hpp) — which is also correct under cross-thread contention, and refuses rather than deadlocks if called on a lane or offload worker.",
      ],
      [
        "check(cond, label)",
        "Example-local. The repo's standalone examples are their own test harness: a file-level failure counter, plus a label that is the claim being made.",
      ],
      [
        "user_message(text)",
        "Example-local shorthand for the Message inside a StartRun{...}.",
      ],
      [
        "text_of(message), make_stream<T>(...)",
        "Real engine API — core/tool_call_extraction.hpp and core/stream.hpp respectively. One caveat on text_of: it concatenates every Text part with NO separator, and its own header says it is for display only. On a fan-in delivery that carries one ContentItem per contributing branch, it silently glues the branches together — read the items per ContentItem when the parts matter, which is exactly why the merged-delivery snippets define their own all_text_of.",
      ],
      [
        "text_message, all_text_of, node_desc, counted, appender, handler",
        "Example-local helpers on the workflow pages. Each is defined in the example file named on the panel, and none is engine API: text_message builds a user Message, all_text_of concatenates a Message's text parts (examples/09_concurrent_workflow.cpp:64), node_desc builds an Executor (examples/23_handoff_mesh.cpp:103), counted wraps a body with an invocation counter, and appender/handler build named worker bodies.",
      ],
      [
        "a page-local using alias, e.g. Session, OrderAgent",
        "A named AgentSession<ChatClientT, StateT, HistoryProviderT> instantiation, so the snippet stays readable. Never engine API.",
      ],
      [
        "using namespace agentengine;",
        "Not enough for the runtime vocabulary. StartRun, ResolveInteraction, SessionCaller, AgentSession and every suspend sentinel live in agentengine::rt:: — so a snippet showing `using namespace agentengine;` and then writing StartRun{...} still needs agentengine::rt::StartRun, or a second using-directive for rt. This is the most common single reason a copied session snippet does not compile.",
      ],
      [
        "caps, store, held",
        "Values the snippet expects you to have built already: a ChatClientCapabilities, a secret store, a CapabilitySet. Where a snippet needs one and does not show it, the page that owns that subject shows the construction.",
      ],
      [
        "set_capabilities(&held)",
        "The one call here that can crash you rather than fail you. It takes a POINTER and remembers it with a no-op deleter — the session does not own the set — so the CapabilitySet must outlive every run on that session. Declare it at the same scope as the session; a set local to a wiring function that returns leaves a dangling pointer, which compiles cleanly and faults on first use.",
      ],
    ],
    foot: "If you want one file you can compile from top to bottom, with its includes and its main(), start with Your first program.",
    footLink: "Your first program",
  },
  vi: {
    kind: "ghi chú",
    summary: "Cách đọc mã trên trang này",
    lead: "Mọi đoạn mã ở đây là một phần trích từ một tệp thật, đã được kiểm thử — thanh tiêu đề của khung mã nêu tên và liên kết tới tệp đó. Điều đó có một hệ quả đáng biết trước khi bạn thử biên dịch: một vài cái tên trong các đoạn mã này là helper riêng của tệp nguồn, không phải API của engine.",
    rows: [
      [
        "drive(task)",
        "Cục bộ trong ví dụ, không phải API của engine. Tương đương thật trong engine là agentengine::rt::block_on(task) (rt/block_on.hpp) — cũng đúng khi có tranh chấp giữa các luồng, và từ chối thay vì deadlock nếu bị gọi trên worker của lane hoặc offload.",
      ],
      [
        "check(cond, label)",
        "Cục bộ trong ví dụ. Các ví dụ độc lập của repo tự làm harness kiểm thử: một bộ đếm lỗi ở mức tệp, cùng một nhãn chính là điều đang được khẳng định.",
      ],
      [
        "user_message(text)",
        "Cách viết tắt cục bộ trong ví dụ cho Message bên trong một StartRun{...}.",
      ],
      [
        "text_of(message), make_stream<T>(...)",
        "API thật của engine — lần lượt ở core/tool_call_extraction.hpp và core/stream.hpp. Một lưu ý về text_of: nó nối mọi phần Text mà KHÔNG có dấu phân cách, và chính header của nó nói rằng nó chỉ dành cho hiển thị. Trên một delivery fan-in mang một ContentItem cho mỗi nhánh đóng góp, nó âm thầm dán các nhánh lại với nhau — hãy đọc từng ContentItem khi các phần đó thực sự quan trọng, và đó chính là lý do các đoạn mã về delivery đã gộp tự định nghĩa all_text_of cho riêng chúng.",
      ],
      [
        "text_message, all_text_of, node_desc, counted, appender, handler",
        "Các helper cục bộ trong ví dụ, xuất hiện ở các trang workflow. Mỗi cái được định nghĩa trong tệp ví dụ ghi trên khung mã, và không cái nào là API của engine: text_message dựng một Message của người dùng, all_text_of nối các phần text của một Message (examples/09_concurrent_workflow.cpp:64), node_desc dựng một Executor (examples/23_handoff_mesh.cpp:103), counted bọc một body với bộ đếm số lần gọi, còn appender/handler dựng các body worker có tên.",
      ],
      [
        "một alias using cục bộ trong trang, ví dụ Session, OrderAgent",
        "Một cách đặt tên cho AgentSession<ChatClientT, StateT, HistoryProviderT> đã được khởi tạo, để đoạn mã dễ đọc. Không bao giờ là API của engine.",
      ],
      [
        "using namespace agentengine;",
        "Không đủ cho phần từ vựng runtime. StartRun, ResolveInteraction, SessionCaller, AgentSession và mọi sentinel tạm dừng đều nằm trong agentengine::rt:: — nên một đoạn mã có `using namespace agentengine;` rồi viết StartRun{...} vẫn cần agentengine::rt::StartRun, hoặc thêm một using-directive cho rt. Đây là lý do phổ biến nhất khiến một đoạn mã session được sao chép không biên dịch được.",
      ],
      [
        "caps, store, held",
        "Các giá trị mà đoạn mã giả định bạn đã dựng sẵn: một ChatClientCapabilities, một secret store, một CapabilitySet. Khi một đoạn mã cần chúng mà không trình bày, trang sở hữu chủ đề đó sẽ trình bày cách dựng.",
      ],
      [
        "set_capabilities(&held)",
        "Lệnh gọi duy nhất ở đây có thể làm bạn crash thay vì trả lỗi. Nó nhận một CON TRỎ và ghi nhớ con trỏ đó với một deleter rỗng — session không sở hữu tập hợp này — nên CapabilitySet phải sống lâu hơn mọi lần chạy trên session đó. Hãy khai báo nó ở cùng phạm vi với session; một tập hợp cục bộ trong một hàm wiring rồi hàm đó return sẽ để lại con trỏ lơ lửng, thứ vẫn biên dịch sạch sẽ và lỗi ngay lần dùng đầu tiên.",
      ],
    ],
    foot: "Nếu bạn muốn một tệp có thể biên dịch từ đầu đến cuối, kể cả các dòng include và hàm main(), hãy bắt đầu với Chương trình đầu tiên của bạn.",
    footLink: "Chương trình đầu tiên của bạn",
  },
};

export function ApiSnippetConventions() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <details className="api-conventions">
      <summary>
        <span className="kind">{t.kind}:</span>
        <span>{t.summary}</span>
      </summary>
      <div className="api-conventions-body">
        <p>{t.lead}</p>
        <dl>
          {t.rows.map(([sym, meaning]) => (
            <div key={sym}>
              <dt>
                <code>{sym}</code>
              </dt>
              <dd>{meaning}</dd>
            </div>
          ))}
        </dl>
        <p>
          {t.foot.replace(t.footLink, "")}
          <a href={`${SITE_BASE}/api/first-app.html`}>{t.footLink}</a>.
        </p>
      </div>
    </details>
  );
}
