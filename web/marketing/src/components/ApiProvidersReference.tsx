import {
  adrRows,
  anthropicBodySnippet,
  anthropicSseSnippet,
  capabilityRows,
  chatRequestSnippet,
  conformerRows,
  credentialSnippet,
  credentialSteps,
  egressRows,
  gapRows,
  gatewaySnippet,
  liveTestRows,
  openaiBodySnippet,
  openaiSseSnippet,
  providerEntries,
  reasoningRows,
  replaySnippet,
} from "../data/providerContent";
import { SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { PvCredentialDiagram, PvSseDiagram, PvWireDiagram } from "./PvDiagrams";
import { RevealGroup, RevealItem } from "./Reveal";
import type { Lang } from "../i18n/LanguageContext";

function entryById(lang: Lang, id: string) {
  const e = providerEntries[lang].find((entry) => entry.id === id);
  if (!e) throw new Error(`missing provider entry: ${id}`);
  return e;
}

function CiteLink({ id }: { id: string }) {
  const { lang } = useLang();
  const e = entryById(lang, id);
  return (
    <a
      className="api-cite"
      href={e.href}
      target="_blank"
      rel="noreferrer"
      style={{ borderTop: "none", paddingTop: 0, marginTop: 18, display: "block" }}
    >
      {e.cite}
    </a>
  );
}

/** Each section opens the same way: the anchor the TOC targets, the section's own eyebrow, the
 * entry's title as an h3, and the entry's body as the standing claim the rest of the section
 * then substantiates. Keeping this in one place means every section's claim comes from
 * providerContent's cited entry list rather than from prose typed inline here. */
function SectionHead({ id, eyebrow, first }: { id: string; eyebrow: string; first?: boolean }) {
  const { lang } = useLang();
  const e = entryById(lang, id);
  const tu = ui[lang];
  return (
    <div
      className="section-head anchor-target"
      id={id}
      style={{ marginTop: first ? 0 : 56, marginBottom: 22, maxWidth: 820 }}
    >
      <span className="eyebrow">{eyebrow}</span>
      <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{e.title}</h3>
      <div className="doc-entry-head" style={{ marginBottom: 14 }}>
        <code className="api-tag">{e.tag}</code>
        <span className={`status-badge status-${e.status}`}>
          {e.status === "real" ? tu.statusRealTested : tu.statusDesignedNotBuilt}
        </span>
      </div>
      <p>{e.body}</p>
    </div>
  );
}

const copy = {
  en: {
    eyebrow: "004 — ChatClient plane",
    headingPrefix: "Model providers,",
    headingHighlight: "one seam and every conformer behind it",
    intro: (
      <>
        004 is titled the <em>ChatClient</em> plane, not the provider plane — the type is{" "}
        <code>ChatClient</code>, and "provider" stays free for the colloquial vendor sense. One
        concept sits between the engine and every inference API, modelled on declared{" "}
        <strong>capabilities</strong> rather than on any vendor's request shape. This page walks
        the whole plane as it exists in this tree: what the concept actually requires, which of
        the twenty declared capability fields anything reads, every conformer and wrapper that
        ships, how a credential reaches an HTTP header without ever being held, what the two
        vendor backends really put on the wire, and — at the end — the parts of 004 that are not
        built.
      </>
    ),
    statBits: "declared capability fields",
    statRead: "actually read by code",
    statConformers: "ChatClient conformers",
    statAdrs: "ADRs governing this plane",

    s1Eyebrow: "core/chat_client.hpp — the concept and its three siblings",
    conceptCols: ["Concept", "Required shape", "Who satisfies it, and why it exists"],
    conceptRows: [
      [
        "ChatClient",
        "capabilities() + chat_stream()",
        "The one every tool and agent is written against. ADR-035 Phase 3 removed chat() from it — a widening, not a deletion: a future backend with only chat_stream() now conforms, and every backend that exists today still has a chat().",
      ],
      [
        "LegacyChatClient",
        "capabilities() + chat() + chat_stream()",
        "Exactly what ChatClient used to require, given a name so it survives the relaxation. RecordingChatClient gates on this, because its body genuinely calls .chat() and gating on ChatClient stopped guaranteeing one — a chat_stream()-only backend would have compiled and then failed with an opaque template error.",
      ],
      [
        "ModelCallGatewayLike",
        "capabilities() + call()",
        "ADR-036's alternate shape. AgentSession::run_model_call() picks between the two with if constexpr, so a raw single backend is completely unaffected. ModelCallGateway is the one conformer and is deliberately NOT a ChatClient.",
      ],
      [
        "HasProducerChatClientId",
        "producer_chat_client_id()",
        "Optional and duck-typed rather than added to the required shape — widening either concept would force every mock in the codebase to grow a method it has no real identity to report. A type that does not satisfy it simply gets no cross-provider Reasoning filtering, never a compile error.",
      ],
    ],
    s1Note: (
      <>
        <strong>The RFC's own §1 signature block is now stricter than the code.</strong> It lists{" "}
        <code>chat()</code> as part of the seam; the concept no longer requires it. This is a case
        where the spec text has not caught up with an executed ADR rather than a case of code
        drifting from a spec — the direction matters, and the header says which one moved.
      </>
    ),

    s2Eyebrow: "004 §2 — the bitset, and what consults it",
    capCols: ["Field", "Type", "What reads it"],
    degradeTitle: "The degradation rule is one pure function",
    degrade: (
      <>
        <code>select_output_schema_strategy(caps)</code> returns{" "}
        <code>native</code> when <code>structured_output_native</code> is set,{" "}
        <code>tool_shaped</code> when only <code>tool_calling</code> is, and{" "}
        <code>parse_and_repair</code> otherwise. It is a pure function of declared capabilities, so
        it is the same decision at every call site rather than re-derived ad hoc — and{" "}
        <code>json_mode</code> is notably not consulted at all. Because{" "}
        <code>parse_and_repair</code> needs no capability bit, it can never fail to produce a
        strategy, which is why 004 §2's "if no fallback exists,{" "}
        <code>register_agent&lt;A&gt;()</code> fails at startup" clause is honestly unreachable
        through it.
      </>
    ),
    reasoningTitle: "reasoning_effort — one ordinal level, two genuinely different native shapes",
    reasoningBody: (
      <>
        The single sampling-adjacent knob carved out of §1's elision (ADR-020), and an abstraction
        each backend maps down rather than a vendor field passed through. OpenAI's{" "}
        <code>minimal</code> is deliberately absent: Ollama has no equivalent, so admitting it
        would make this OpenAI's enum with a rename. <code>nullopt</code> and <code>off</code> are
        different requests — no opinion versus explicitly disable — and every surveyed backend can
        express both.
      </>
    ),
    reasoningCols: ["ChatRequest::reasoning_effort", "OpenAI-compatible", "Anthropic"],
    s2Note: (
      <>
        <strong>Anthropic enforces its vendor's own floor here, client-side, on purpose.</strong>{" "}
        Anthropic documents <code>budget_tokens &gt;= 1024</code> and{" "}
        <code>budget_tokens &lt; max_tokens</code>. A gateway may not enforce them — OpenRouter was
        measured returning HTTP 200 for both <code>budget_tokens == max_tokens</code> and{" "}
        <code>budget_tokens == 512</code>, while <code>api.anthropic.com</code> rejects both.
        Sending a request that works through the lenient hop and breaks against the strict one is
        exactly the silent divergence this project's conventions exist to prevent, so the client
        fails closed with <code>anthropic.thinking_budget_unsatisfiable</code> and names the
        numbers and the remedy.
      </>
    ),

    s3Eyebrow: "The inventory — verified against include/, not against comments",
    conformerCols: ["Type", "Which concept", "What it is"],
    s3Note: (
      <>
        <strong>Three wrappers named in older comments are gone.</strong> Nothing in{" "}
        <code>include/agentengine/core/</code> defines <code>FailoverChatClient</code>,{" "}
        <code>ResilientChatClient</code> or <code>MiddlewareChatClient</code>; there are no such
        headers. They were removed on 2026-08-12, the day ADR-036 landed, because giving all three{" "}
        <code>chat_stream()</code> parity had a confirmed use-after-free hazard and this repo had
        shipped nowhere, so there was no deprecation cost to weigh against deleting them. If you
        find one of those names in a file-top comment in this tree — including in{" "}
        <code>chat_client.hpp</code>'s own note about <code>idempotency_key</code> — that comment
        is stale.
      </>
    ),
    s3ForwardNote: (
      <>
        Neither gateway type is wired to anything above by itself — this page stops at the
        type. For the object built from these two, plugged straight into{" "}
        <code>AgentSession</code>'s first template slot, see{" "}
        <a href={`${SITE_BASE}/api/runtime.html#chat-clients`}>
          AgentSession &amp; ChatClient — Chat clients
        </a>.
      </>
    ),

    s4Eyebrow: "004 §1 / 018 §4 — resolution at the point of use",

    s5Eyebrow: "ADR-016 — one resolution implementation, two policies",
    egressCols: ["", "WASM guest egress path", "Host-initiated provider path"],
    s5Note: (
      <>
        <strong>The trade under <code>plaintext_http</code>, stated plainly.</strong> The request
        still carries the provider credential — <code>Authorization: Bearer …</code> for the
        OpenAI-compatible backend, <code>x-api-key</code> for Anthropic — and that header crosses
        the wire in clear. On loopback this is uncontroversial: an attacker who can read loopback
        traffic already owns the process. On any real network it is a credential disclosure. That
        is why it is opt-in, non-default, and a named enumerator at the call site rather than a
        bool a reviewer would have to open a header to interpret.
      </>
    ),

    s6Eyebrow: "004 §3 — request shaping, both directions",
    s6Sub: "The portable request",
    s6SubOpenAI: "Rendered by the OpenAI-compatible backend",
    s6SubAnthropic: "Rendered by the Anthropic backend",

    s7Eyebrow: "ADR-019 — decoded and pushed as the bytes arrive",
    s7SubOpenAI: "OpenAI-compatible: data: lines only",
    s7SubAnthropic: "Anthropic: named events",

    s8Eyebrow: "004 §5-§6 / I5 — the recorded seam",
    s8Note: (
      <>
        <strong>Two honest limits on this seam.</strong> First, replay does no request matching at
        all: <code>ReplayChatClient</code> ignores the live request and{" "}
        <code>EffectContext</code> entirely and always serves its one recording, so there is no
        prompt comparison and no cassette-miss detection — only a call-shape check. Second, 004 §7
        G3 is proven on each half separately (the recorder preserves per-chunk timing and the
        terminal; the player reproduces exact inter-chunk deltas against an injected clock) but{" "}
        <em>never across the seam</em>: no test in this tree records a real call and then replays
        that same recording, and every replay test constructs its{" "}
        <code>ChatCallRecording</code> by hand.
      </>
    ),

    s9Eyebrow: "004 §7 G1 — evidence, and how it is gated",
    liveGateLabel: "How it is gated",
    liveProvesLabel: "What it proves",

    s10Eyebrow: "004 §2-§8 — the honest ledger",
    gapCols: ["Area", "What 004 asks for", "What this tree has"],
    adrTitle: "Which ADRs actually govern this plane",
    adrBody: (
      <>
        There is no single "ChatClient design" ADR — 004 itself is the governing spec, and the ADRs
        amend it in specific places. Two of the areas a reader would most expect an ADR for have
        none at all, which is worth saying rather than implying otherwise.
      </>
    ),
    adrCols: ["ADR", "Title", "What it actually decided"],
  },
  vi: {
    eyebrow: "004 — Bình diện ChatClient",
    headingPrefix: "Nhà cung cấp model,",
    headingHighlight: "một ranh giới và mọi conformer đứng sau nó",
    intro: (
      <>
        004 được đặt tên là bình diện <em>ChatClient</em>, không phải bình diện nhà cung cấp — kiểu
        dữ liệu là <code>ChatClient</code>, và chữ "provider" được giữ lại cho nghĩa thông tục là
        hãng cung cấp model. Một concept duy nhất nằm giữa engine và mọi API suy luận, được mô
        hình hóa theo các <strong>năng lực</strong> được khai báo chứ không theo hình dạng request
        của bất kỳ hãng nào. Trang này đi qua toàn bộ bình diện đúng như nó đang tồn tại trong cây
        mã: concept thực sự đòi hỏi những gì, trong hai mươi trường năng lực được khai báo thì có
        gì được đọc, mọi conformer và wrapper đã có, cách một credential đi tới một HTTP header mà
        không bao giờ bị giữ lại, hai backend nhà cung cấp thật sự đặt gì lên dây, và — ở cuối
        trang — những phần của 004 chưa được xây.
      </>
    ),
    statBits: "trường năng lực được khai báo",
    statRead: "thực sự được mã nguồn đọc",
    statConformers: "conformer của ChatClient",
    statAdrs: "ADR chi phối bình diện này",

    s1Eyebrow: "core/chat_client.hpp — concept và ba concept anh em",
    conceptCols: ["Concept", "Hình dạng bắt buộc", "Ai thỏa mãn, và vì sao nó tồn tại"],
    conceptRows: [
      [
        "ChatClient",
        "capabilities() + chat_stream()",
        "Concept mà mọi tool và agent đều được viết dựa vào. ADR-035 Phase 3 đã gỡ chat() khỏi nó — đây là mở rộng, không phải xóa bỏ: một backend tương lai chỉ có chat_stream() giờ đã tuân theo được, và mọi backend đang tồn tại vẫn có chat().",
      ],
      [
        "LegacyChatClient",
        "capabilities() + chat() + chat_stream()",
        "Chính xác những gì ChatClient từng đòi hỏi, được đặt cho một cái tên để sống sót qua đợt nới lỏng. RecordingChatClient ràng buộc theo concept này, vì thân hàm của nó thực sự gọi .chat() và việc ràng buộc theo ChatClient đã không còn bảo đảm điều đó — một backend chỉ có chat_stream() sẽ biên dịch qua rồi hỏng với một lỗi template khó hiểu.",
      ],
      [
        "ModelCallGatewayLike",
        "capabilities() + call()",
        "Hình dạng thay thế của ADR-036. AgentSession::run_model_call() chọn giữa hai hình dạng bằng if constexpr, nên một backend đơn lẻ thô hoàn toàn không bị ảnh hưởng. ModelCallGateway là conformer duy nhất và cố ý KHÔNG phải một ChatClient.",
      ],
      [
        "HasProducerChatClientId",
        "producer_chat_client_id()",
        "Tùy chọn và nhận diện kiểu duck-typing thay vì đưa vào hình dạng bắt buộc — mở rộng một trong hai concept kia sẽ buộc mọi mock trong codebase phải mọc thêm một phương thức mà chúng không có danh tính thật nào để báo. Một kiểu không thỏa mãn nó đơn giản là không được lọc Reasoning chéo giữa các provider, chứ không bao giờ gây lỗi biên dịch.",
      ],
    ],
    s1Note: (
      <>
        <strong>Chính khối chữ ký ở §1 của RFC giờ đây khắt khe hơn mã nguồn.</strong> Nó liệt kê{" "}
        <code>chat()</code> như một phần của ranh giới; concept thì không còn đòi hỏi nữa. Đây là
        trường hợp văn bản đặc tả chưa bắt kịp một ADR đã được thực thi, chứ không phải mã nguồn
        trôi khỏi đặc tả — chiều của sự lệch pha là quan trọng, và header nói rõ bên nào đã dịch
        chuyển.
      </>
    ),

    s2Eyebrow: "004 §2 — bộ bit, và ai tra cứu nó",
    capCols: ["Trường", "Kiểu", "Ai đọc nó"],
    degradeTitle: "Quy tắc suy giảm là một hàm thuần túy duy nhất",
    degrade: (
      <>
        <code>select_output_schema_strategy(caps)</code> trả về <code>native</code> khi có{" "}
        <code>structured_output_native</code>, <code>tool_shaped</code> khi chỉ có{" "}
        <code>tool_calling</code>, và <code>parse_and_repair</code> trong các trường hợp còn lại.
        Đây là một hàm thuần túy của các năng lực được khai báo, nên nó cho cùng một quyết định ở
        mọi điểm gọi thay vì được suy diễn lại tùy tiện — và đáng chú ý là <code>json_mode</code>{" "}
        hoàn toàn không được tra cứu. Vì <code>parse_and_repair</code> không cần bit năng lực nào,
        nó không bao giờ có thể thất bại trong việc đưa ra một chiến lược, và đó là lý do mệnh đề
        "nếu không có fallback nào, <code>register_agent&lt;A&gt;()</code> thất bại lúc khởi động"
        của 004 §2 là không thể chạm tới qua hàm này.
      </>
    ),
    reasoningTitle: "reasoning_effort — một mức thứ tự, hai hình dạng gốc thực sự khác nhau",
    reasoningBody: (
      <>
        Núm điều chỉnh cận-sampling duy nhất được tách khỏi phần lược bỏ của §1 (ADR-020), và là
        một trừu tượng mà mỗi backend tự ánh xạ xuống chứ không phải một trường của hãng được
        chuyển tiếp nguyên xi. Mức <code>minimal</code> của OpenAI cố ý vắng mặt: Ollama không có
        mức tương đương, nên thừa nhận nó sẽ biến đây thành enum của OpenAI chỉ đổi tên.{" "}
        <code>nullopt</code> và <code>off</code> là hai yêu cầu khác nhau — không có ý kiến, so
        với tắt hẳn một cách tường minh — và mọi backend đã khảo sát đều biểu đạt được cả hai.
      </>
    ),
    reasoningCols: ["ChatRequest::reasoning_effort", "Tương thích OpenAI", "Anthropic"],
    s2Note: (
      <>
        <strong>Anthropic tự thực thi sàn của chính hãng ngay tại đây, phía client, một cách cố ý.</strong>{" "}
        Anthropic tài liệu hóa <code>budget_tokens &gt;= 1024</code> và{" "}
        <code>budget_tokens &lt; max_tokens</code>. Một gateway có thể không thực thi chúng —
        OpenRouter đã được đo là trả HTTP 200 cho cả <code>budget_tokens == max_tokens</code> lẫn{" "}
        <code>budget_tokens == 512</code>, trong khi <code>api.anthropic.com</code> từ chối cả
        hai. Gửi một request chạy được qua chặng dễ dãi rồi hỏng ở chặng khắt khe chính là kiểu
        phân kỳ âm thầm mà các quy ước của dự án này tồn tại để ngăn chặn, nên client từ chối đóng
        với <code>anthropic.thinking_budget_unsatisfiable</code> và nêu rõ các con số cùng cách
        khắc phục.
      </>
    ),

    s3Eyebrow: "Bản kiểm kê — đối chiếu với include/, không phải với các chú thích",
    conformerCols: ["Kiểu", "Thuộc concept nào", "Nó là gì"],
    s3Note: (
      <>
        <strong>Ba wrapper được nêu tên trong các chú thích cũ đã biến mất.</strong> Không có gì
        trong <code>include/agentengine/core/</code> định nghĩa <code>FailoverChatClient</code>,{" "}
        <code>ResilientChatClient</code> hay <code>MiddlewareChatClient</code>; không có header
        nào như vậy. Chúng bị gỡ bỏ ngày 2026-08-12, đúng ngày ADR-036 được đưa vào, vì cho cả ba
        ngang bằng <code>chat_stream()</code> đã bị xác nhận có nguy cơ use-after-free, và kho mã
        này chưa phát hành ra đâu cả nên không có chi phí ngừng-dùng nào để cân nhắc so với việc
        xóa hẳn. Nếu bạn thấy một trong ba cái tên đó trong một chú thích đầu tệp ở cây mã này —
        kể cả trong chính ghi chú của <code>chat_client.hpp</code> về{" "}
        <code>idempotency_key</code> — thì chú thích đó đã cũ.
      </>
    ),
    s3ForwardNote: (
      <>
        Tự bản thân hai kiểu gateway này chưa được đấu nối vào đâu cả — trang này dừng lại ở
        định nghĩa kiểu. Muốn xem đối tượng được xây từ cả hai, cắm thẳng vào vị trí tham số
        template đầu tiên của <code>AgentSession</code>, xem{" "}
        <a href={`${SITE_BASE}/api/runtime.html#chat-clients`}>
          AgentSession &amp; ChatClient — Chat clients
        </a>.
      </>
    ),

    s4Eyebrow: "004 §1 / 018 §4 — phân giải ngay tại điểm sử dụng",

    s5Eyebrow: "ADR-016 — một phần cài đặt phân giải, hai chính sách",
    egressCols: ["", "Đường egress của guest WASM", "Đường provider do host khởi tạo"],
    s5Note: (
      <>
        <strong>Cái giá khi dùng <code>plaintext_http</code>, nói thẳng.</strong> Request vẫn mang
        theo credential của nhà cung cấp — <code>Authorization: Bearer …</code> cho backend tương
        thích OpenAI, <code>x-api-key</code> cho Anthropic — và header đó đi qua dây ở dạng rõ.
        Trên loopback thì điều này không có gì phải bàn: kẻ tấn công đọc được lưu lượng loopback
        thì vốn đã làm chủ tiến trình. Trên bất kỳ mạng thật nào, đó là một vụ lộ credential. Vì
        vậy nó chỉ bật khi khai báo rõ, không phải mặc định, và là một enumerator có tên ngay tại
        điểm khởi tạo thay vì một bool mà người duyệt mã phải mở header ra mới hiểu.
      </>
    ),

    s6Eyebrow: "004 §3 — định hình request, cả hai phía",
    s6Sub: "Request có tính di động",
    s6SubOpenAI: "Được backend tương thích OpenAI dựng ra",
    s6SubAnthropic: "Được backend Anthropic dựng ra",

    s7Eyebrow: "ADR-019 — giải mã và đẩy đi ngay khi byte tới nơi",
    s7SubOpenAI: "Tương thích OpenAI: chỉ các dòng data:",
    s7SubAnthropic: "Anthropic: sự kiện có tên",

    s8Eyebrow: "004 §5-§6 / I5 — ranh giới được ghi lại",
    s8Note: (
      <>
        <strong>Hai giới hạn được nói thẳng ở ranh giới này.</strong> Thứ nhất, việc phát lại hoàn
        toàn không đối chiếu request: <code>ReplayChatClient</code> bỏ qua cả request đang chạy
        lẫn <code>EffectContext</code> và luôn phục vụ đúng một bản ghi của nó, nên không có so
        sánh prompt và không có phát hiện "trượt cassette" — chỉ có một kiểm tra hình dạng lệnh
        gọi. Thứ hai, 004 §7 G3 được chứng minh trên từng nửa một cách riêng rẽ (bộ ghi giữ được
        thời gian theo từng chunk cùng trạng thái kết thúc; bộ phát lại tái tạo chính xác khoảng
        chênh giữa các chunk dựa trên một đồng hồ được tiêm vào) nhưng{" "}
        <em>chưa bao giờ qua ranh giới</em>: không bài kiểm thử nào trong cây mã ghi một lệnh gọi
        thật rồi phát lại chính bản ghi đó, và mọi bài kiểm thử phát lại đều tự tay dựng{" "}
        <code>ChatCallRecording</code> của mình.
      </>
    ),

    s9Eyebrow: "004 §7 G1 — bằng chứng, và cách nó được kiểm soát",
    liveGateLabel: "Được kiểm soát thế nào",
    liveProvesLabel: "Chứng minh điều gì",

    s10Eyebrow: "004 §2-§8 — bản kê trung thực",
    gapCols: ["Hạng mục", "004 đòi hỏi gì", "Cây mã này có gì"],
    adrTitle: "Những ADR nào thực sự chi phối bình diện này",
    adrBody: (
      <>
        Không có một ADR nào mang tên "thiết kế ChatClient" — chính 004 là đặc tả chi phối, còn các
        ADR sửa đổi nó ở những chỗ cụ thể. Hai lĩnh vực mà người đọc dễ trông đợi nhất là có ADR
        thì lại hoàn toàn không có, và điều đó đáng nói thẳng thay vì để hiểu lầm.
      </>
    ),
    adrCols: ["ADR", "Tiêu đề", "Nó thực sự quyết định điều gì"],
  },
};

export function ApiProvidersReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];

  return (
    <section className="section" id="providers">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 820 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {tu.statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
          <div className="stat-row">
            <div className="stat">
              <div className="num grad-text">20</div>
              <div className="label">{t.statBits}</div>
            </div>
            <div className="stat">
              <div className="num grad-text">7</div>
              <div className="label">{t.statRead}</div>
            </div>
            <div className="stat">
              <div className="num grad-text">4</div>
              <div className="label">{t.statConformers}</div>
            </div>
            <div className="stat">
              <div className="num grad-text">8</div>
              <div className="label">{t.statAdrs}</div>
            </div>
          </div>
        </div>

        {/* ---- 1. The seam ------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="chat-client-seam" eyebrow={t.s1Eyebrow} first />
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.conceptCols]}
              templateColumns="0.9fr 1fr 2.4fr"
              rows={t.conceptRows.map((r) => [<code key="a">{r[0]}</code>, <code key="b">{r[1]}</code>, r[2]])}
            />
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ borderLeftColor: "var(--accent-purple)" }}>{t.s1Note}</p>
          </RevealItem>
          <RevealItem>
            <CiteLink id="chat-client-seam" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. Capabilities ---------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="capabilities" eyebrow={t.s2Eyebrow} />
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.capCols]}
              templateColumns="1.1fr 0.7fr 2.4fr"
              rows={capabilityRows[lang].map((r) => [
                <code key="a">{r.name}</code>,
                <code key="b">{r.type}</code>,
                <span key="c" style={{ color: r.read ? "var(--text-dim)" : "var(--text-faint)" }}>
                  {r.reader}
                </span>,
              ])}
            />
          </RevealItem>
          <RevealItem>
            <div className="section-head" style={{ marginTop: 34, marginBottom: 0, maxWidth: 820 }}>
              <h4 style={{ fontSize: "1.05rem", margin: "0 0 10px" }}>{t.degradeTitle}</h4>
              <p>{t.degrade}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="section-head" style={{ marginTop: 34, marginBottom: 18, maxWidth: 820 }}>
              <h4 style={{ fontSize: "1.05rem", margin: "0 0 10px" }}>{t.reasoningTitle}</h4>
              <p>{t.reasoningBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.reasoningCols]}
              templateColumns="0.8fr 1.1fr 1.9fr"
              rows={reasoningRows[lang].map((r) => [
                <code key="a">{r.level}</code>,
                <code key="b">{r.openai}</code>,
                r.anthropic,
              ])}
            />
          </RevealItem>
          <RevealItem>
            <p className="gs-note">{t.s2Note}</p>
          </RevealItem>
          <RevealItem>
            <CiteLink id="capabilities" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Conformers ------------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="conformers" eyebrow={t.s3Eyebrow} />
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.conformerCols]}
              templateColumns="1.1fr 0.9fr 2.4fr"
              rows={conformerRows[lang].map((r) => [
                <span key="a">
                  <code>{r.name}</code>
                  <span className="api-cite" style={{ borderTop: "none", paddingTop: 4 }}>
                    {r.file}
                  </span>
                </span>,
                r.shape,
                r.note,
              ])}
            />
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ borderLeftColor: "var(--accent-pink)" }}>{t.s3Note}</p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="model_call_gateway.hpp">{highlightCpp(gatewaySnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s3ForwardNote}</p>
          </RevealItem>
          <RevealItem>
            <CiteLink id="conformers" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Credentials ----------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="credentials" eyebrow={t.s4Eyebrow} />
          </RevealItem>
          <RevealItem>
            <PvCredentialDiagram />
          </RevealItem>
          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px", marginTop: 24 }}>
              {credentialSteps[lang].map((s) => (
                <div className="ladder-step" key={s.index}>
                  <span className="ladder-index">{s.index}</span>
                  <div>
                    <h4>{s.title}</h4>
                    <p>{s.body}</p>
                  </div>
                </div>
              ))}
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="openai/chat_client.hpp + trust/secret.hpp">
              {highlightCpp(credentialSnippet)}
            </CodePanel>
          </RevealItem>
          <RevealItem>
            <CiteLink id="credentials" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. Transport ------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="transport" eyebrow={t.s5Eyebrow} />
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.egressCols]}
              templateColumns="0.7fr 1.6fr 1.9fr"
              rows={egressRows[lang].map((r) => [<strong key="a">{r.aspect}</strong>, r.guest, r.host])}
            />
          </RevealItem>
          <RevealItem>
            <p className="gs-note">{t.s5Note}</p>
          </RevealItem>
          <RevealItem>
            <CiteLink id="transport" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Wire bodies ----------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="wire-bodies" eyebrow={t.s6Eyebrow} />
          </RevealItem>
          <RevealItem>
            <PvWireDiagram />
          </RevealItem>
          <RevealItem>
            <div className="spec-layout" style={{ marginTop: 34 }}>
              <div>
                <span className="eyebrow" style={{ display: "block", marginBottom: 10 }}>{t.s6Sub}</span>
                <CodePanel filename="core/chat_client.hpp">
                  {highlightCpp(chatRequestSnippet)}
                </CodePanel>
              </div>
              <div>
                <span className="eyebrow" style={{ display: "block", marginBottom: 10 }}>{t.s6SubOpenAI}</span>
                <CodePanel filename="POST /v1/chat/completions">
                  {highlightCpp(openaiBodySnippet)}
                </CodePanel>
              </div>
              <div>
                <span className="eyebrow" style={{ display: "block", marginBottom: 10 }}>{t.s6SubAnthropic}</span>
                <CodePanel filename="POST /v1/messages">
                  {highlightCpp(anthropicBodySnippet)}
                </CodePanel>
              </div>
            </div>
          </RevealItem>
          <RevealItem>
            <CiteLink id="wire-bodies" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 7. Streaming ------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="streaming" eyebrow={t.s7Eyebrow} />
          </RevealItem>
          <RevealItem>
            <PvSseDiagram />
          </RevealItem>
          <RevealItem>
            <div className="spec-layout" style={{ marginTop: 34 }}>
              <div>
                <span className="eyebrow" style={{ display: "block", marginBottom: 10 }}>{t.s7SubOpenAI}</span>
                <CodePanel filename="openai/chat_client.hpp — split_sse_data_events">
                  {highlightCpp(openaiSseSnippet)}
                </CodePanel>
              </div>
              <div>
                <span className="eyebrow" style={{ display: "block", marginBottom: 10 }}>{t.s7SubAnthropic}</span>
                <CodePanel filename="anthropic/chat_client.hpp — split_sse_named_events">
                  {highlightCpp(anthropicSseSnippet)}
                </CodePanel>
              </div>
            </div>
          </RevealItem>
          <RevealItem>
            <CiteLink id="streaming" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 8. Recording & replay ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="determinism" eyebrow={t.s8Eyebrow} />
          </RevealItem>
          <RevealItem>
            <CodePanel filename="recording_chat_client.hpp + replay_chat_client.hpp">
              {highlightCpp(replaySnippet)}
            </CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ borderLeftColor: "var(--accent-pink)" }}>{t.s8Note}</p>
          </RevealItem>
          <RevealItem>
            <CiteLink id="determinism" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 9. Live evidence ----------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="live-evidence" eyebrow={t.s9Eyebrow} />
          </RevealItem>
          <RevealItem>
            <div className="glass" style={{ padding: "6px 22px", borderRadius: "var(--radius-lg)" }}>
              {liveTestRows[lang].map((r, i) => (
                <div className="flow-stage" key={r.file}>
                  <span className="flow-stage-index">{String(i + 1).padStart(2, "0")}</span>
                  <div>
                    <h4>
                      <code>{r.file}</code>
                    </h4>
                    <p>
                      <strong>{t.liveGateLabel}. </strong>
                      {r.gate}
                    </p>
                    <p style={{ marginBottom: 0 }}>
                      <strong>{t.liveProvesLabel}. </strong>
                      {r.proves}
                    </p>
                  </div>
                </div>
              ))}
            </div>
          </RevealItem>
          <RevealItem>
            <CiteLink id="live-evidence" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 10. Gaps -------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <SectionHead id="gaps" eyebrow={t.s10Eyebrow} />
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.gapCols]}
              templateColumns="0.9fr 1.4fr 1.9fr"
              rows={gapRows[lang].map((r) => [<strong key="a">{r.area}</strong>, r.asked, r.built])}
            />
          </RevealItem>
          <RevealItem>
            <div className="section-head" style={{ marginTop: 46, marginBottom: 18, maxWidth: 820 }}>
              <h4 style={{ fontSize: "1.05rem", margin: "0 0 10px" }}>{t.adrTitle}</h4>
              <p>{t.adrBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.adrCols]}
              templateColumns="0.5fr 1.2fr 2.3fr"
              rows={adrRows[lang].map((r) => [
                <code key="a">{r.id}</code>,
                <span key="b">
                  {r.title}
                  <span className="api-cite" style={{ borderTop: "none", paddingTop: 4 }}>
                    {r.file}
                  </span>
                </span>,
                r.governs,
              ])}
            />
          </RevealItem>
          <RevealItem>
            <CiteLink id="gaps" />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
