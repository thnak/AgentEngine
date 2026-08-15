// Hand-authored inline SVG for the Model providers page. No diagram library and no new
// dependency: three figures, each drawn from the real code it describes, using the .diagram /
// .dg-* scaffold in index.css so they inherit the site's own theme tokens.
//
// Language-neutral strings (JSON keys, C++ identifiers, header names, wire-format fields) are
// module-level constants below and appear identically in both languages; only prose is translated.

import { useLang } from "../i18n/LanguageContext";

// ---- Diagram 1: one ChatRequest, two wire bodies ----------------------------------------------

const REQUEST_FIELDS = [
  "messages",
  "tools  → ToolDescriptor[] (006)",
  "output_schema_json",
  "reasoning_effort (ADR-020)",
  "idempotency_key",
];

const CAPS_FIELDS = [
  "reasoning          → both backends",
  "prompt_caching     → Anthropic only",
  "max_output_tokens  → Anthropic only",
  "…16 others         → declared, unread",
];

const OPENAI_LINES = [
  'model · messages   (role::system stays an ordinary message)',
  'tools[]  →  {"type":"function","function":{name, description, parameters}}',
  'response_format  →  json_schema · strict:true · additionalProperties FORCED false',
  'reasoning_effort  →  "none" | "low" | "medium" | "high"',
  'stream_options.include_usage  →  streaming path only, or usage never arrives',
  'never emitted: max_tokens · tool_choice · cache_control · any sampling param',
];

const ANTHROPIC_LINES = [
  'model · messages   (system messages hoisted OUT into a `system` field)',
  'max_tokens  ←  caps.max_output_tokens, else kDefaultMaxTokens = 4096 — REQUIRED',
  'tools[]  →  flat {name, description, input_schema}; cache_control on the LAST one',
  'output_config  →  {format:{type:"json_schema", schema}} — no forcing of any kind',
  'reasoning_effort  →  thinking:{type:"enabled", budget_tokens: 25/50/75% of max}',
  'refuses: >4 cache_control blocks · budget ≥ max_tokens · reasoning without the bit',
];

const wireCopy = {
  en: {
    title: "One ChatRequest, rendered by two translation functions",
    reqTitle: "ChatRequest",
    capsTitle: "ChatClientCapabilities",
    openai: "openai::detail::build_request_body  →  POST /v1/chat/completions",
    anthropic: "anthropic::detail::build_request_body  →  POST /v1/messages",
    openaiHeader: "header: Authorization: Bearer <lease->reveal_text()>",
    anthropicHeader: "headers: x-api-key: <lease->reveal_text()> + anthropic-version: 2023-06-01",
    legendReq: "ChatRequest fields",
    legendCaps: "ChatClientCapabilities (read at translation time, not probed)",
    caption:
      "Drawn from protocol/openai/chat_client.hpp:264-349 and protocol/anthropic/chat_client.hpp:376-482. The same request object produces two bodies that share almost no structure — which is the point of the seam: neither backend passes a vendor field through, and the caller never learns which wire format it is on.",
  },
  vi: {
    title: "Một ChatRequest, được hai hàm dịch dựng lại",
    reqTitle: "ChatRequest",
    capsTitle: "ChatClientCapabilities",
    openai: "openai::detail::build_request_body  →  POST /v1/chat/completions",
    anthropic: "anthropic::detail::build_request_body  →  POST /v1/messages",
    openaiHeader: "header: Authorization: Bearer <lease->reveal_text()>",
    anthropicHeader: "headers: x-api-key: <lease->reveal_text()> + anthropic-version: 2023-06-01",
    legendReq: "Các trường của ChatRequest",
    legendCaps: "ChatClientCapabilities (đọc lúc dịch, không dò tìm)",
    caption:
      "Vẽ từ protocol/openai/chat_client.hpp:264-349 và protocol/anthropic/chat_client.hpp:376-482. Cùng một đối tượng request sinh ra hai thân gói tin gần như không chung cấu trúc nào — và đó chính là mục đích của ranh giới này: không backend nào chuyển tiếp nguyên xi một trường của nhà cung cấp, còn phía gọi thì không bao giờ biết mình đang chạy trên định dạng dây nào.",
  },
};

export function PvWireDiagram() {
  const { lang } = useLang();
  const t = wireCopy[lang];
  return (
    <figure className="diagram glass" style={{ margin: "28px 0 0" }}>
      <svg viewBox="0 0 1040 470" role="img" aria-label={t.title}>
        <title>{t.title}</title>

        {/* inputs */}
        <rect className="dg-box-strong" x="16" y="60" width="288" height="168" rx="10" />
        <text className="dg-label" x="32" y="86">{t.reqTitle}</text>
        {REQUEST_FIELDS.map((line, i) => (
          <text className="dg-mono" x="32" y={110 + i * 20} key={line}>{line}</text>
        ))}

        <rect className="dg-box" x="16" y="262" width="288" height="152" rx="10" />
        <text className="dg-label" x="32" y="288">{t.capsTitle}</text>
        {CAPS_FIELDS.map((line, i) => (
          <text className="dg-mono" x="32" y={312 + i * 20} key={line}>{line}</text>
        ))}

        {/* connectors */}
        <path className="dg-line" d="M304 120 C 328 120, 328 96, 352 96" />
        <path className="dg-line" d="M304 190 C 328 190, 328 320, 352 320" />
        <path className="dg-line dg-lifeline" d="M304 300 C 328 300, 328 150, 352 150" />
        <path className="dg-line dg-lifeline" d="M304 372 L 352 372" />

        {/* OpenAI lane */}
        <rect className="dg-box" x="352" y="24" width="672" height="200" rx="10" />
        <text className="dg-label" x="372" y="50">{t.openai}</text>
        {OPENAI_LINES.map((line, i) => (
          <text className="dg-mono" x="372" y={76 + i * 20} key={line}>{line}</text>
        ))}
        <text className="dg-faint" x="372" y="206">{t.openaiHeader}</text>

        {/* Anthropic lane */}
        <rect className="dg-box" x="352" y="248" width="672" height="200" rx="10" />
        <text className="dg-label" x="372" y="274">{t.anthropic}</text>
        {ANTHROPIC_LINES.map((line, i) => (
          <text className="dg-mono" x="372" y={300 + i * 20} key={line}>{line}</text>
        ))}
        <text className="dg-faint" x="372" y="430">{t.anthropicHeader}</text>
      </svg>
      <div className="diagram-legend">
        <span>
          <i style={{ borderTopColor: "var(--border-strong)" }} />
          {t.legendReq}
        </span>
        <span>
          <i style={{ borderTopColor: "var(--border-strong)", borderTopStyle: "dashed" }} />
          {t.legendCaps}
        </span>
      </div>
      <figcaption className="diagram-caption">{t.caption}</figcaption>
    </figure>
  );
}

// ---- Diagram 2: two SSE framings, one decode path ---------------------------------------------

const OPENAI_EVENTS: Array<[string, string]> = [
  ['data: {"choices":[{"delta":{"content":"Ha"}}]}', "one Text update, pushed the moment it arrives"],
  ['data: {"choices":[{"delta":{"content":"noi"}}]}', "1:1 with the vendor's own chunk boundary"],
  ['data: {… tool_calls[0].function.arguments: "{\\"loc" }', "held: index 0 accumulates, nothing emitted yet"],
  ['data: {"choices":[], "usage":{…}}', "captured BEFORE the empty-choices skip"],
  ["data: [DONE]", "finish(): the assembled ToolCall is emitted whole"],
];

const ANTHROPIC_EVENTS: Array<[string, string]> = [
  ["event: message_start", 'data: {"message":{"usage":{"input_tokens":41,…}}}'],
  ["event: content_block_start", 'data: {"index":1,"content_block":{"type":"tool_use",…}}'],
  ["event: content_block_delta", 'data: {"index":0,"delta":{"type":"text_delta","text":"Ha"}}'],
  ["event: content_block_delta", 'data: {"index":1,"delta":{"type":"input_json_delta",…}}'],
  ["event: message_delta", 'data: {"usage":{"output_tokens":9}}   ← CUMULATIVE, overwritten'],
];

const sseCopy = {
  en: {
    title: "Two SSE framings feeding one decode path",
    openaiHead: "OpenAI-compatible — data: lines only",
    openaiSub: "split_sse_data_events · event:/id:/comment lines ignored · one JSON object per line",
    anthropicHead: "Anthropic — named events",
    anthropicSub: "split_sse_named_events · an event: line PAIRED with the data: line after it",
    sharedTitle: "One shared decode path — ChunkedBodyDecoder → SseEventFramer → StreamingUpdateAccumulator",
    shared: [
      "feed(fragment) returns whichever ChatResponseUpdates are complete RIGHT NOW — ADR-019",
      "one-item hold-back, so is_final marks the genuinely LAST update rather than guessing",
      "the one-shot parse is built ON TOP of this accumulator, so the two paths cannot drift",
    ],
    legendEvent: "one SSE event as delivered",
    legendNote: "what the accumulator does with it",
    caption:
      "Drawn from protocol/openai/chat_client.hpp:565-758 and protocol/anthropic/chat_client.hpp:717-976. Anthropic's usage is cumulative on the wire — output_tokens is overwritten by each message_delta, never summed — and a text_delta is surfaced as Text only when its own index was started as a text block, which is ADR-035's guard against a non-compliant Anthropic-wire gateway.",
  },
  vi: {
    title: "Hai kiểu đóng khung SSE cùng đổ vào một đường giải mã",
    openaiHead: "Tương thích OpenAI — chỉ các dòng data:",
    openaiSub: "split_sse_data_events · bỏ qua các dòng event:/id:/chú thích · mỗi dòng một đối tượng JSON",
    anthropicHead: "Anthropic — sự kiện có tên",
    anthropicSub: "split_sse_named_events · một dòng event: ĐI KÈM dòng data: ngay sau nó",
    sharedTitle: "Một đường giải mã dùng chung — ChunkedBodyDecoder → SseEventFramer → StreamingUpdateAccumulator",
    shared: [
      "feed(fragment) trả về những ChatResponseUpdate đã hoàn chỉnh NGAY LÚC NÀY — ADR-019",
      "giữ lại một mục, nên is_final đánh dấu đúng bản cập nhật CUỐI thật sự chứ không phải phỏng đoán",
      "bản phân tích một-lần được dựng TRÊN NỀN bộ tích lũy này, nên hai đường không thể trôi lệch",
    ],
    legendEvent: "một sự kiện SSE như khi nhận được",
    legendNote: "bộ tích lũy làm gì với nó",
    caption:
      "Vẽ từ protocol/openai/chat_client.hpp:565-758 và protocol/anthropic/chat_client.hpp:717-976. Usage của Anthropic là tích lũy trên dây — output_tokens bị mỗi message_delta ghi đè chứ không cộng dồn — và một text_delta chỉ được đưa ra thành Text khi chính chỉ số của nó được bắt đầu như một khối text, đây là biện pháp gia cố của ADR-035 trước một gateway nói giao thức Anthropic nhưng không tuân thủ.",
  },
};

export function PvSseDiagram() {
  const { lang } = useLang();
  const t = sseCopy[lang];
  return (
    <figure className="diagram glass" style={{ margin: "28px 0 0" }}>
      <svg viewBox="0 0 1040 540" role="img" aria-label={t.title}>
        <title>{t.title}</title>

        {/* column headers */}
        <rect className="dg-box-strong" x="16" y="16" width="496" height="52" rx="10" />
        <text className="dg-label" x="32" y="38">{t.openaiHead}</text>
        <text className="dg-sub" x="32" y="57">{t.openaiSub}</text>

        <rect className="dg-box-strong" x="528" y="16" width="496" height="52" rx="10" />
        <text className="dg-label" x="544" y="38">{t.anthropicHead}</text>
        <text className="dg-sub" x="544" y="57">{t.anthropicSub}</text>

        {/* lifelines */}
        <line className="dg-lifeline" x1="30" y1="80" x2="30" y2="390" />
        <line className="dg-lifeline" x1="542" y1="80" x2="542" y2="390" />

        {/* OpenAI events */}
        {OPENAI_EVENTS.map(([wire, note], i) => (
          <g key={wire}>
            <rect className="dg-box" x="44" y={84 + i * 60} width="468" height="52" rx="8" />
            <text className="dg-mono" x="60" y={84 + i * 60 + 22}>{wire}</text>
            <text className="dg-faint" x="60" y={84 + i * 60 + 41}>{"→ " + note}</text>
          </g>
        ))}

        {/* Anthropic events */}
        {ANTHROPIC_EVENTS.map(([evt, data], i) => (
          <g key={evt + i}>
            <rect className="dg-box" x="556" y={84 + i * 60} width="468" height="52" rx="8" />
            <text className="dg-mono" x="572" y={84 + i * 60 + 22}>{evt}</text>
            <text className="dg-faint" x="572" y={84 + i * 60 + 41}>{data}</text>
          </g>
        ))}

        {/* shared decode path */}
        <path className="dg-line" d="M278 390 L 278 404" />
        <path className="dg-line" d="M790 390 L 790 404" />
        <rect className="dg-box-strong" x="16" y="404" width="1008" height="118" rx="10" />
        <text className="dg-label" x="36" y="432">{t.sharedTitle}</text>
        {t.shared.map((line, i) => (
          <text className="dg-mono" x="36" y={458 + i * 20} key={line}>{line}</text>
        ))}
      </svg>
      <div className="diagram-legend">
        <span>
          <i style={{ borderTopColor: "var(--border-strong)" }} />
          {t.legendEvent}
        </span>
        <span>
          <i style={{ borderTopColor: "var(--text-faint)", borderTopStyle: "dotted" }} />
          {t.legendNote}
        </span>
      </div>
      <figcaption className="diagram-caption">{t.caption}</figcaption>
    </figure>
  );
}

// ---- Diagram 3: a credential reaching the wire -------------------------------------------------

const credentialCopy = {
  en: {
    title: "A credential's whole life: from a name in config to one HTTP header",
    steps: [
      {
        head: "SecretRef",
        mono: "{ name }",
        l1: "The only credential-shaped member either vendor client has — nothing resolvable is stored.",
        l2: "A config dump or a crash dump taken between two calls finds a name, and nothing else.",
      },
      {
        head: "chat(request, ctx)",
        mono: "store_.resolve(ref, ctx)",
        l1: "Resolution starts HERE, on every call, against the caller's own EffectContext — never at construction.",
        l2: "018 §4 grants a native seam backend no exemption from \"never read into a config struct at startup\".",
      },
      {
        head: "cap::Secret gate",
        mono: "contains(cap::Secret)",
        l1: "Exact name match against the granted set; any granted TTL covers this zero-TTL request.",
        l2: "A null capability pointer is a denial, not a permissive default. This gate runs first.",
      },
      {
        head: "SecretSource::get",
        mono: "env / <dir>/<name>",
        l1: "An environment variable, or <dir>/<name> with one trailing newline stripped.",
        l2: "The bytes land in a Secret: heap-allocated, wiped in its destructor, bytes() its only accessor.",
      },
      {
        head: "reveal_text()",
        mono: "Authorization: Bearer",
        l1: "Or x-api-key for Anthropic. One loudly-named accessor, called on the line that builds the header.",
        l2: "The lease dies when the call returns, wiping the buffer — no member on either client holds it.",
      },
    ],
    denyTitle: "secret.not_granted",
    denyMono: "failure_class::policy",
    denyL1: "Returned before any SecretSource is read and before any socket is opened.",
    denyL2: "A run whose capability set omits this name never reaches the network at all.",
    legendPath: "the granted path",
    legendDeny: "the fail-closed path",
    caption:
      "Drawn from trust/secret.hpp:196-321 and protocol/openai/chat_client.hpp:937-990. Nothing between step 1 and step 5 is a policy decision the model can influence: the grant is host policy, and a run whose capability set omits cap::Secret{name} never reaches the network at all — proven live at OR-OAI-8/OR-ANT-8 and LC-7, against endpoints that were up and would have answered.",
  },
  vi: {
    title: "Trọn đời một credential: từ một cái tên trong cấu hình tới một HTTP header",
    steps: [
      {
        head: "SecretRef",
        mono: "{ name }",
        l1: "Thành viên duy nhất mang hình dạng credential của cả hai client — không lưu lại thứ gì phân giải được.",
        l2: "Một bản config dump hay crash dump lấy giữa hai lệnh gọi chỉ tìm thấy một cái tên.",
      },
      {
        head: "chat(request, ctx)",
        mono: "store_.resolve(ref, ctx)",
        l1: "Việc phân giải bắt đầu TẠI ĐÂY, ở mọi lệnh gọi, dựa trên EffectContext của phía gọi — không phải lúc khởi tạo.",
        l2: "018 §4 không miễn trừ cho một backend seam gốc quy tắc \"không đọc vào struct cấu hình lúc khởi động\".",
      },
      {
        head: "Cổng cap::Secret",
        mono: "contains(cap::Secret)",
        l1: "Khớp tên chính xác với tập quyền đã cấp; mọi TTL đã cấp đều bao phủ yêu cầu TTL bằng không.",
        l2: "Con trỏ capability rỗng là một sự từ chối, không phải mặc định dễ dãi. Cổng này chạy trước tiên.",
      },
      {
        head: "SecretSource::get",
        mono: "env / <dir>/<name>",
        l1: "Một biến môi trường, hoặc <dir>/<name> với một ký tự xuống dòng ở cuối bị loại bỏ.",
        l2: "Các byte nằm trong một Secret: cấp phát trên heap, xóa sạch trong destructor, bytes() là accessor duy nhất.",
      },
      {
        head: "reveal_text()",
        mono: "Authorization: Bearer",
        l1: "Hoặc x-api-key với Anthropic. Một accessor duy nhất tên thật rõ, gọi trên dòng dựng header.",
        l2: "Lease chết khi lệnh gọi trả về, xóa sạch bộ đệm — không thành viên nào của hai client giữ nó.",
      },
    ],
    denyTitle: "secret.not_granted",
    denyMono: "failure_class::policy",
    denyL1: "Trả về trước khi đọc bất kỳ SecretSource nào và trước khi mở bất kỳ socket nào.",
    denyL2: "Một run có tập capability thiếu cái tên này hoàn toàn không chạm tới mạng.",
    legendPath: "đường được cấp quyền",
    legendDeny: "đường từ chối đóng",
    caption:
      "Vẽ từ trust/secret.hpp:196-321 và protocol/openai/chat_client.hpp:937-990. Không có bước nào từ 1 tới 5 là một quyết định chính sách mà model có thể tác động: việc cấp quyền là chính sách của host, và một run có tập capability thiếu cap::Secret{name} hoàn toàn không chạm tới mạng — điều này được chứng minh trực tiếp ở OR-OAI-8/OR-ANT-8 và LC-7, đối diện những endpoint đang chạy và lẽ ra đã trả lời.",
  },
};

export function PvCredentialDiagram() {
  const { lang } = useLang();
  const t = credentialCopy[lang];
  return (
    <figure className="diagram glass" style={{ margin: "28px 0 0" }}>
      <svg viewBox="0 0 1040 424" role="img" aria-label={t.title}>
        <title>{t.title}</title>

        {/* Vertical ladder rather than a horizontal chain: each step's explanation is a real
            sentence, and only a full-width row has room for one without clipping. */}
        {t.steps.map((s, i) => {
          const y = 16 + i * 68;
          return (
            <g key={s.head}>
              <text className="dg-faint" x="16" y={y + 34}>{"0" + (i + 1)}</text>
              <rect
                className={i === 2 ? "dg-box-strong" : "dg-box"}
                x="48"
                y={y}
                width="248"
                height="56"
                rx="8"
              />
              <text className="dg-label" x="64" y={y + 24}>{s.head}</text>
              <text className="dg-mono" x="64" y={y + 44}>{s.mono}</text>
              <text className="dg-sub" x="320" y={y + 24}>{s.l1}</text>
              <text className="dg-sub" x="320" y={y + 44}>{s.l2}</text>
              {i < 4 ? <path className="dg-line" d={`M172 ${y + 56} L 172 ${y + 68}`} /> : null}
            </g>
          );
        })}

        {/* fail-closed branch off step 03 */}
        <path className="dg-line dg-lifeline" d="M172 208 C 36 208, 36 384, 48 384" />
        <rect className="dg-box" x="48" y="356" width="248" height="56" rx="8" />
        <text className="dg-label" x="64" y="380">{t.denyTitle}</text>
        <text className="dg-mono" x="64" y="400">{t.denyMono}</text>
        <text className="dg-sub" x="320" y="380">{t.denyL1}</text>
        <text className="dg-sub" x="320" y="400">{t.denyL2}</text>
      </svg>
      <div className="diagram-legend">
        <span>
          <i style={{ borderTopColor: "var(--border-strong)" }} />
          {t.legendPath}
        </span>
        <span>
          <i style={{ borderTopColor: "var(--border)", borderTopStyle: "dashed" }} />
          {t.legendDeny}
        </span>
      </div>
      <figcaption className="diagram-caption">{t.caption}</figcaption>
    </figure>
  );
}
