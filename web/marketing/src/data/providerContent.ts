// Content for the Model providers page (api/providers.html) — 004 §1-§7, the ChatClient plane.
//
// Same honesty rule as apiContent.ts, and this page needs it more than most: 004 declares an
// 18-bit capability set, four backend classes and a reliability section, and this tree implements
// a real but strictly narrower slice of each. Every claim below was read out of the working tree
// (headers, tests, CMakeLists, decisions/) at the line cited — never from an RFC's own prose, and
// never from a file-top comment that a later ADR has since overtaken (several here have; where a
// banner and the code disagree, the code is what this page reports, and the divergence is named).
//
// Bilingual (EN/VI): every translatable field is a Record<Lang, T>. Code snippets, identifiers,
// wire-format field names, RFC/ADR numbers, file paths and proper nouns stay identical in both
// languages — only surrounding prose is translated.

import { gh, type ApiEntry } from "./apiContent";
import type { Lang } from "../i18n/LanguageContext";

export interface ProviderSection {
  id: string;
  label: string;
}

// Every id here is the `id` attribute of a <section>/anchor-target that ApiProvidersReference
// actually renders — this feeds the right-rail scroll-spy TOC.
export const providerSections: Record<Lang, ProviderSection[]> = {
  en: [
    { id: "chat-client-seam", label: "The ChatClient seam" },
    { id: "capabilities", label: "Capabilities & degradation" },
    { id: "conformers", label: "Every shipped conformer" },
    { id: "credentials", label: "A credential reaching the wire" },
    { id: "transport", label: "Egress, TLS, address policy" },
    { id: "wire-bodies", label: "One request, two wire bodies" },
    { id: "streaming", label: "Two SSE framings" },
    { id: "determinism", label: "Recording & replay" },
    { id: "live-evidence", label: "Live-endpoint evidence" },
    { id: "gaps", label: "Narrower than 004" },
  ],
  vi: [
    { id: "chat-client-seam", label: "Ranh giới ChatClient" },
    { id: "capabilities", label: "Năng lực & suy giảm" },
    { id: "conformers", label: "Mọi conformer đã có" },
    { id: "credentials", label: "Đường một credential ra dây" },
    { id: "transport", label: "Egress, TLS, chính sách địa chỉ" },
    { id: "wire-bodies", label: "Một request, hai thân gói tin" },
    { id: "streaming", label: "Hai kiểu đóng khung SSE" },
    { id: "determinism", label: "Ghi & phát lại" },
    { id: "live-evidence", label: "Bằng chứng từ endpoint thật" },
    { id: "gaps", label: "Hẹp hơn 004" },
  ],
};

// One entry per section, carrying that section's own primary citation. Rendered by the page's
// CiteLink, exactly the pattern ApiRuntimeReference established for runtimeEntries.
export const providerEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "chat-client-seam",
      status: "real",
      tag: "concept ChatClient",
      title: "Two required expressions, and nothing else",
      body:
        "A concept, never a base class — no virtual dispatch on the model-call path. As of ADR-035 Phase 3 the required shape is exactly capabilities() and chat_stream(); chat() was deliberately dropped from it, which WIDENS what conforms rather than deleting anything (every backend that exists still has a chat()). Three sibling concepts sit beside it: LegacyChatClient is the pre-relaxation shape, kept because RecordingChatClient's body genuinely calls .chat() and gating on ChatClient stopped guaranteeing that; ModelCallGatewayLike is ADR-036's alternate shape AgentSession accepts instead; HasProducerChatClientId is optional and duck-typed, so a mock that has no real backend identity simply gets no cross-provider filtering.",
      cite: "include/agentengine/core/chat_client.hpp:171",
      href: gh("include/agentengine/core/chat_client.hpp"),
    },
    {
      id: "capabilities",
      status: "real",
      tag: "ChatClientCapabilities",
      title: "Declared, never probed — and only seven of the twenty are read by anything",
      body:
        "004 §2's full bitset is present: 18 bools plus context_window and max_output_tokens, all defaulting to false/0, with no per-field doc comments and no constructor — a pure aggregate the deployment fills in from its own config. What the code actually consults is much narrower, and the table below says so field by field rather than implying that declaring a bit does something. The degradation rule itself is one pure function, select_output_schema_strategy: native if structured_output_native, else tool_shaped if tool_calling, else parse_and_repair. It can never fail — parse_and_repair needs no bit — so 004 §2's 'if no fallback exists, register_agent<A>() fails at startup' clause is honestly unreachable through this function, and the header says exactly that instead of faking a failure case.",
      cite: "include/agentengine/core/chat_client.hpp:35",
      href: gh("include/agentengine/core/chat_client.hpp"),
    },
    {
      id: "conformers",
      status: "real",
      tag: "OpenAIChatClient · AnthropicChatClient · ReplayChatClient · RecordingChatClient",
      title: "Four conformers and two gateways — three fewer wrappers than the comments claim",
      body:
        "The current set is smaller than several file-top comments in this tree still say. FailoverChatClient, ResilientChatClient and MiddlewareChatClient no longer exist — no such header is in include/agentengine/core/, and grep finds them only in stale prose and in the ADRs that removed them. They were deleted on 2026-08-12, the same day ADR-036 landed ModelCallGateway<Primary, Fallback...> and MiddlewareModelCallGateway<Inner, Ms...>, because this repo had shipped nowhere and there was no deprecation cost to justify keeping them. Neither gateway is a ChatClient: they have no chat()/chat_stream() at all, only call(), which has to be a real coroutine so a middleware hook can be co_awaited.",
      cite: "include/agentengine/core/model_call_gateway.hpp:120",
      href: gh("include/agentengine/core/model_call_gateway.hpp"),
    },
    {
      id: "credentials",
      status: "real",
      tag: "SecretRef · SecretStore::resolve() · SecretLease",
      title: "The backend holds a name; the value exists only inside one call",
      body:
        "Both vendor clients store SecretRef api_key_ref_ and a Store const& — there is no SecretLease member anywhere. Resolution happens inside chat() and chat_stream(), against the caller's EffectContext, at the point of use (004 §1 / 018 §4). It fails closed first: without a granted cap::Secret naming exactly this ref, resolve() returns failure_class::policy / secret.not_granted before any source is touched, and a null capability pointer is a denial, not a permissive default. SecretLease is move-only by static_assert; the underlying Secret zeroizes on destruction and has no std::string conversion and no operator<<, so reveal_text() is the one, deliberately greppable place a credential becomes text — immediately before it becomes an HTTP header.",
      cite: "include/agentengine/trust/secret.hpp:245",
      href: gh("include/agentengine/trust/secret.hpp"),
    },
    {
      id: "transport",
      status: "real",
      tag: "ProviderTransport · perform_provider_https_exchange · resolve_host",
      title: "The SSRF table is guest-path-only, and that was a decision, not an oversight",
      body:
        "ADR-016 split one resolver into two over a single shared implementation so they cannot drift: resolve_and_validate keeps the blocked-range table for the WASM guest egress path, resolve_host drops it for host-initiated provider calls. The reasoning is about who influences the destination — a guest-supplied host is the textbook SSRF target, while a provider host is a field the deployment itself configured and, by I3, can never be derived from model output. Both keep ADR-011's resolve-once-connect-to-a-verified-literal discipline, so DNS rebinding is defended on both paths. Transport is a named enumerator, never inferred from a scheme and never probed: tls (mbedTLS, TLS 1.2 floor, real hostname verification against a pinned CA bundle compiled in at configure time) or plaintext_http, opt-in, for a local llama.cpp/vLLM/Ollama server with no certificate — on which the credential header is readable on the wire, which the ADR states plainly rather than burying.",
      cite: "include/agentengine/sandbox/provider_http_client.hpp:60",
      href: gh("include/agentengine/sandbox/provider_http_client.hpp"),
    },
    {
      id: "wire-bodies",
      status: "real",
      tag: "build_request_body()",
      title: "One ChatRequest, two genuinely different bodies",
      body:
        "Neither backend passes a vendor field through. The same ChatRequest becomes a Chat Completions body whose tools are wrapped in {\"type\":\"function\",\"function\":{...}} and whose output schema becomes response_format with additionalProperties forced false and strict:true — and an Anthropic Messages body with flat {name, description, input_schema} tools, an output_config wrapper with no additionalProperties forcing, and a max_tokens the OpenAI backend never emits at all. Both refuse rather than degrade on one thing: a non-off reasoning_effort against a backend that does not declare the reasoning bit fails with failure_class::contract, because 004 §2 has no declared fallback for it and silently dropping the field is exactly what that rule forbids.",
      cite: "include/agentengine/protocol/anthropic/chat_client.hpp:376",
      href: gh("include/agentengine/protocol/anthropic/chat_client.hpp"),
    },
    {
      id: "streaming",
      status: "real",
      tag: "split_sse_data_events · split_sse_named_events",
      title: "Two SSE framings, one decoder shape, real per-chunk delivery",
      body:
        "An OpenAI-compatible stream is data:-only lines, one compact JSON object each; Anthropic's is named events, an event: line paired with the following data: line. Both feed the same shared primitives (ChunkedBodyDecoder, SseEventFramer) and the same StreamingUpdateAccumulator shape, and in both the one-shot parse is implemented ON TOP of the incremental accumulator rather than beside it, so the two paths cannot drift into two decoders. Since ADR-019 the bytes are decoded and pushed as they arrive — perform_provider_streaming_exchange delivers fragments to an on_body callback and each ready update goes straight onto the credit-controlled ring. Both vendor files still carry a Phase-D banner describing the older buffer-then-replay behaviour; the code no longer does that, and this page reports the code.",
      cite: "include/agentengine/protocol/anthropic/chat_client.hpp:722",
      href: gh("include/agentengine/protocol/anthropic/chat_client.hpp"),
    },
    {
      id: "determinism",
      status: "real",
      tag: "RecordingChatClient<Inner> · ReplayChatClient",
      title: "The I5 seam — chunk boundaries preserved in time, not just in order",
      body:
        "RecordingChatClient wraps any LegacyChatClient, forwards capabilities() untouched, returns the inner outcome completely unchanged, and records BOTH success and failure. It does no file I/O at all — the caller supplies the sink, and the default one discards. On the streaming path it keeps recording to completion even after the consumer drops the stream, and it calls the sink BEFORE latching the producer's terminal, so a recording is committed before any caller can observe that the call finished. ReplayChatClient then sleeps the delta between successive RecordedChunk::elapsed_since_start before each push, which is what makes 004 §7 G3 ('identical chunk boundaries') true in time and not merely in delivery order. Its capabilities() are supplied by the caller, never inferred from the recording, and feeding a unary recording to chat_stream() is a named error rather than a silent degradation.",
      cite: "include/agentengine/core/replay_chat_client.hpp:93",
      href: gh("include/agentengine/core/replay_chat_client.hpp"),
    },
    {
      id: "live-evidence",
      status: "real",
      tag: "ctest -L live-network",
      title: "What a canned loopback server can never prove",
      body:
        "test_openai_chat_client_live.cpp and test_anthropic_chat_client_live.cpp run over a real TLS socket, but against a canned loopback server: every byte the client receives was written by the test itself, so they prove plumbing and parsing and nothing about whether what this project SENDS is well-formed. A canned server drains and replies unconditionally; a real one returns HTTP 400. Two files close that gap — one against a hosted service over real TLS with the real DNS resolver and the real vendored CA bundle, one against a local llama.cpp server over the opt-in plaintext transport. Both SKIP with exit 0 when their environment variable is unset, both carry the ctest label live-network, and nothing in either asserts on model CONTENT: every assertion is structural, because a live model is nondeterministic and I5 is respected here by exclusion, not by pretending the output is stable.",
      cite: "tests/test_openrouter_live_e2e.cpp:16",
      href: gh("tests/test_openrouter_live_e2e.cpp"),
    },
    {
      id: "gaps",
      status: "design",
      tag: "004 §2-§7 — what is not built",
      title: "The parts of 004 this tree does not implement",
      body:
        "Two of 004 §3's four backend classes have no code at all: a remote A2A agent used where a model would be, and a ChatClient shipped as a WASM plugin through the ae:provider world. The Responses API half of the OpenAI row is likewise unbuilt — only Chat Completions exists. §2's degradation rule is enforced but not recorded, because no trace sink exists yet. §4's stable idempotency key is generated and carried, but no backend turns it into a request header, and the breaker key is per-backend-position, not per {tenant, provider, model, SecretRef}. §5's per-run TokenBudget<N> is real; the per-tenant and per-principal ceilings, the cost metric and the configuration-driven pricing tables are not, and Usage::cost_estimate is round-tripped by the recording codec but never populated by any backend. §6's privacy modes for recordings do not exist. §7's G4 performance budget has no bench.",
      cite: "004-Model-Provider-Plane.md",
      href: gh("004-Model-Provider-Plane.md"),
    },
  ],
  vi: [
    {
      id: "chat-client-seam",
      status: "real",
      tag: "concept ChatClient",
      title: "Hai biểu thức bắt buộc, và không gì khác",
      body:
        "Một concept, không bao giờ là lớp cơ sở — không có virtual dispatch trên đường gọi model. Kể từ ADR-035 Phase 3, hình dạng bắt buộc đúng là capabilities() và chat_stream(); chat() đã được cố ý loại khỏi đó, việc này MỞ RỘNG những gì có thể tuân theo chứ không xóa bỏ điều gì (mọi backend đang tồn tại vẫn có chat()). Ba concept anh em nằm cạnh nó: LegacyChatClient là hình dạng trước khi nới lỏng, được giữ lại vì thân hàm của RecordingChatClient thực sự gọi .chat() và việc chỉ ràng buộc theo ChatClient đã không còn bảo đảm điều đó; ModelCallGatewayLike là hình dạng thay thế của ADR-036 mà AgentSession chấp nhận; HasProducerChatClientId là tùy chọn và nhận diện theo kiểu duck-typing, nên một mock không có danh tính backend thật đơn giản là không được lọc chéo giữa các provider.",
      cite: "include/agentengine/core/chat_client.hpp:171",
      href: gh("include/agentengine/core/chat_client.hpp"),
    },
    {
      id: "capabilities",
      status: "real",
      tag: "ChatClientCapabilities",
      title: "Khai báo, không bao giờ dò tìm — và chỉ bảy trong hai mươi trường được đọc",
      body:
        "Bộ bit đầy đủ của 004 §2 đã có mặt: 18 bool cộng context_window và max_output_tokens, tất cả mặc định false/0, không có chú thích cho từng trường và không có constructor — một aggregate thuần túy mà deployment tự điền từ cấu hình của mình. Những gì mã nguồn thực sự đọc thì hẹp hơn nhiều, và bảng bên dưới nói rõ điều đó theo từng trường thay vì ngụ ý rằng khai báo một bit là đã có tác dụng. Bản thân quy tắc suy giảm là một hàm thuần túy duy nhất, select_output_schema_strategy: native nếu có structured_output_native, ngược lại tool_shaped nếu có tool_calling, ngược lại parse_and_repair. Nó không bao giờ có thể thất bại — parse_and_repair không cần bit nào — nên mệnh đề 'nếu không có fallback nào, register_agent<A>() thất bại ngay lúc khởi động' của 004 §2 là không thể chạm tới qua hàm này, và header nói thẳng điều đó thay vì bịa ra một trường hợp lỗi giả.",
      cite: "include/agentengine/core/chat_client.hpp:35",
      href: gh("include/agentengine/core/chat_client.hpp"),
    },
    {
      id: "conformers",
      status: "real",
      tag: "OpenAIChatClient · AnthropicChatClient · ReplayChatClient · RecordingChatClient",
      title: "Bốn conformer và hai gateway — ít hơn ba wrapper so với những gì các chú thích còn nói",
      body:
        "Tập hợp hiện tại nhỏ hơn những gì vài chú thích đầu tệp trong cây mã này vẫn khẳng định. FailoverChatClient, ResilientChatClient và MiddlewareChatClient không còn tồn tại — không có header nào như vậy trong include/agentengine/core/, và grep chỉ tìm thấy chúng trong văn bản đã cũ và trong chính các ADR đã gỡ bỏ chúng. Chúng bị xóa ngày 2026-08-12, đúng ngày ADR-036 đưa ModelCallGateway<Primary, Fallback...> và MiddlewareModelCallGateway<Inner, Ms...> vào, bởi kho mã này chưa phát hành ra đâu cả nên không có chi phí ngừng-dùng-rồi-di-trú nào biện minh cho việc giữ lại. Cả hai gateway đều không phải ChatClient: chúng hoàn toàn không có chat()/chat_stream(), chỉ có call(), và call() buộc phải là một coroutine thật để một hook middleware có thể được co_await.",
      cite: "include/agentengine/core/model_call_gateway.hpp:120",
      href: gh("include/agentengine/core/model_call_gateway.hpp"),
    },
    {
      id: "credentials",
      status: "real",
      tag: "SecretRef · SecretStore::resolve() · SecretLease",
      title: "Backend giữ một cái tên; giá trị chỉ tồn tại bên trong một lệnh gọi",
      body:
        "Cả hai client nhà cung cấp đều lưu SecretRef api_key_ref_ và một Store const& — không có thành viên SecretLease ở bất kỳ đâu. Việc phân giải diễn ra bên trong chat() và chat_stream(), dựa trên EffectContext của caller, ngay tại điểm sử dụng (004 §1 / 018 §4). Nó từ chối đóng trước tiên: nếu không có một cap::Secret được cấp và gọi đúng tên ref này, resolve() trả về failure_class::policy / secret.not_granted trước khi chạm tới bất kỳ nguồn nào, và một con trỏ capability rỗng là một sự từ chối, không phải một mặc định dễ dãi. SecretLease không thể sao chép, được bảo đảm bằng static_assert; Secret bên dưới tự xóa sạch khi hủy, không có chuyển đổi sang std::string và không có operator<<, nên reveal_text() là nơi DUY NHẤT, cố ý dễ grep, mà một credential trở thành văn bản — ngay trước khi nó trở thành một HTTP header.",
      cite: "include/agentengine/trust/secret.hpp:245",
      href: gh("include/agentengine/trust/secret.hpp"),
    },
    {
      id: "transport",
      status: "real",
      tag: "ProviderTransport · perform_provider_https_exchange · resolve_host",
      title: "Bảng chặn SSRF chỉ áp dụng cho đường guest, và đó là một quyết định, không phải sơ suất",
      body:
        "ADR-016 tách một resolver thành hai, trên cùng một phần cài đặt chung để chúng không thể trôi lệch khỏi nhau: resolve_and_validate giữ bảng dải địa chỉ bị chặn cho đường egress của guest WASM, còn resolve_host bỏ bảng đó cho các lệnh gọi provider do host khởi tạo. Lập luận nằm ở chỗ AI ảnh hưởng được đến đích đến — một host do guest cung cấp chính là mục tiêu SSRF kinh điển, trong khi host của provider là một trường do chính deployment cấu hình và, theo I3, không bao giờ có thể bắt nguồn từ đầu ra của model. Cả hai đều giữ kỷ luật phân-giải-một-lần-rồi-kết-nối-tới-một-địa-chỉ-đã-xác-minh của ADR-011, nên DNS rebinding vẫn được phòng thủ trên cả hai đường. Transport là một enumerator có tên, không bao giờ được suy ra từ scheme và không bao giờ được dò: tls (mbedTLS, sàn TLS 1.2, xác minh hostname thật dựa trên bộ CA đã ghim và biên dịch sẵn lúc configure) hoặc plaintext_http, chỉ khi khai báo rõ, dành cho máy chủ llama.cpp/vLLM/Ollama cục bộ không có chứng chỉ — trên đó header chứa credential đọc được trên dây, điều mà ADR nói thẳng thay vì che giấu.",
      cite: "include/agentengine/sandbox/provider_http_client.hpp:60",
      href: gh("include/agentengine/sandbox/provider_http_client.hpp"),
    },
    {
      id: "wire-bodies",
      status: "real",
      tag: "build_request_body()",
      title: "Một ChatRequest, hai thân gói tin thực sự khác nhau",
      body:
        "Không backend nào chuyển tiếp nguyên xi một trường của nhà cung cấp. Cùng một ChatRequest trở thành một thân Chat Completions với tools được bọc trong {\"type\":\"function\",\"function\":{...}} và output schema trở thành response_format với additionalProperties bị ép thành false cùng strict:true — và một thân Anthropic Messages với tools phẳng dạng {name, description, input_schema}, một lớp bọc output_config không hề ép additionalProperties, cùng một max_tokens mà backend OpenAI hoàn toàn không phát ra. Cả hai đều từ chối thay vì suy giảm ở đúng một điểm: một reasoning_effort khác off gửi tới backend không khai báo bit reasoning sẽ thất bại với failure_class::contract, vì 004 §2 không có fallback được khai báo nào cho nó và việc lặng lẽ bỏ trường đó chính là điều quy tắc ấy cấm.",
      cite: "include/agentengine/protocol/anthropic/chat_client.hpp:376",
      href: gh("include/agentengine/protocol/anthropic/chat_client.hpp"),
    },
    {
      id: "streaming",
      status: "real",
      tag: "split_sse_data_events · split_sse_named_events",
      title: "Hai kiểu đóng khung SSE, một hình dạng bộ giải mã, phân phát thật theo từng chunk",
      body:
        "Một luồng tương thích OpenAI chỉ gồm các dòng data:, mỗi dòng một đối tượng JSON gọn; luồng của Anthropic gồm các sự kiện có tên, một dòng event: đi kèm dòng data: ngay sau đó. Cả hai đều dùng chung các nguyên thủy giống nhau (ChunkedBodyDecoder, SseEventFramer) và cùng một hình dạng StreamingUpdateAccumulator, và ở cả hai, bản phân tích một-lần được cài đặt TRÊN NỀN bộ tích lũy tăng dần chứ không phải song song với nó, nên hai đường không thể trôi thành hai bộ giải mã khác nhau. Kể từ ADR-019, các byte được giải mã và đẩy đi ngay khi tới nơi — perform_provider_streaming_exchange giao các mảnh cho một callback on_body và mỗi bản cập nhật đã sẵn sàng đi thẳng lên vòng đệm có kiểm soát tín dụng. Cả hai tệp nhà cung cấp vẫn còn một banner từ Phase D mô tả hành vi cũ là gom-hết-rồi-phát-lại; mã nguồn không còn làm vậy nữa, và trang này thuật lại mã nguồn.",
      cite: "include/agentengine/protocol/anthropic/chat_client.hpp:722",
      href: gh("include/agentengine/protocol/anthropic/chat_client.hpp"),
    },
    {
      id: "determinism",
      status: "real",
      tag: "RecordingChatClient<Inner> · ReplayChatClient",
      title: "Ranh giới I5 — biên chunk được giữ nguyên theo thời gian, không chỉ theo thứ tự",
      body:
        "RecordingChatClient bọc bất kỳ LegacyChatClient nào, chuyển tiếp capabilities() nguyên vẹn, trả lại kết quả bên trong hoàn toàn không thay đổi, và ghi lại CẢ thành công lẫn thất bại. Nó hoàn toàn không thực hiện I/O tệp — caller cung cấp sink, và sink mặc định thì vứt bỏ. Trên đường streaming, nó tiếp tục ghi cho tới khi hoàn tất ngay cả khi phía tiêu thụ đã buông luồng, và nó gọi sink TRƯỚC khi chốt trạng thái kết thúc của producer, nên một bản ghi đã được cam kết trước khi bất kỳ caller nào có thể quan sát thấy lệnh gọi kết thúc. ReplayChatClient sau đó ngủ đúng khoảng chênh lệch giữa các RecordedChunk::elapsed_since_start liên tiếp trước mỗi lần đẩy, và đó là điều làm cho 004 §7 G3 ('biên chunk giống hệt') đúng theo thời gian chứ không chỉ theo thứ tự phân phát. capabilities() của nó do caller cung cấp, không bao giờ suy ra từ bản ghi, và đưa một bản ghi dạng unary vào chat_stream() là một lỗi có tên chứ không phải một sự suy giảm âm thầm.",
      cite: "include/agentengine/core/replay_chat_client.hpp:93",
      href: gh("include/agentengine/core/replay_chat_client.hpp"),
    },
    {
      id: "live-evidence",
      status: "real",
      tag: "ctest -L live-network",
      title: "Điều mà một máy chủ loopback đóng hộp không bao giờ chứng minh được",
      body:
        "test_openai_chat_client_live.cpp và test_anthropic_chat_client_live.cpp chạy qua một socket TLS thật, nhưng đối diện một máy chủ loopback đóng hộp: mọi byte client nhận được đều do chính bài kiểm thử viết ra, nên chúng chứng minh phần đấu nối và phân tích cú pháp chứ không nói gì về việc những gì dự án này GỬI ĐI có đúng dạng hay không. Một máy chủ đóng hộp đọc hết rồi trả lời vô điều kiện; một máy chủ thật trả về HTTP 400. Hai tệp lấp khoảng trống đó — một đối diện dịch vụ được lưu trữ (hosted) qua TLS thật với resolver DNS thật và bộ CA đã ghim thật, một đối diện máy chủ llama.cpp cục bộ qua transport plaintext phải khai báo rõ mới dùng. Cả hai đều BỎ QUA với mã thoát 0 khi biến môi trường của chúng chưa được đặt, cả hai mang nhãn ctest live-network, và không có gì trong hai tệp khẳng định về NỘI DUNG của model: mọi khẳng định đều mang tính cấu trúc, vì một model chạy thật là bất định và I5 ở đây được tôn trọng bằng cách loại trừ, chứ không phải bằng cách vờ như đầu ra là ổn định.",
      cite: "tests/test_openrouter_live_e2e.cpp:16",
      href: gh("tests/test_openrouter_live_e2e.cpp"),
    },
    {
      id: "gaps",
      status: "design",
      tag: "004 §2-§7 — những phần chưa xây",
      title: "Những phần của 004 mà cây mã này không cài đặt",
      body:
        "Hai trong bốn lớp backend của 004 §3 hoàn toàn chưa có mã: một agent A2A ở xa được dùng thay chỗ của một model, và một ChatClient phát hành dưới dạng plugin WASM qua world ae:provider. Nửa Responses API của dòng OpenAI cũng chưa được xây — chỉ có Chat Completions. Quy tắc suy giảm ở §2 được thực thi nhưng không được ghi lại, vì chưa có trace sink nào tồn tại. Khóa idempotency ổn định ở §4 được sinh ra và mang theo, nhưng không backend nào biến nó thành một header của request, và khóa của circuit breaker là theo vị trí backend, không phải theo {tenant, provider, model, SecretRef}. TokenBudget<N> theo từng run ở §5 là thật; các trần theo tenant và theo principal, chỉ số chi phí và các bảng giá điều khiển bằng cấu hình thì chưa, còn Usage::cost_estimate được bộ mã hóa bản ghi chuyển qua chuyển lại nhưng không backend nào từng điền vào. Các chế độ riêng tư cho bản ghi ở §6 không tồn tại. Ngân sách hiệu năng G4 ở §7 chưa có bench nào.",
      cite: "004-Model-Provider-Plane.md",
      href: gh("004-Model-Provider-Plane.md"),
    },
  ],
};

// ---- Capabilities: the honest "what actually reads this" column ------------------------------

export interface CapabilityRow {
  name: string;
  type: string;
  read: boolean;
  reader: string;
}

// `read` is literal: does ANY non-test file in include/ or src/ consult this field? Verified by
// grepping every `caps.<field>` / `capabilities().<field>` use in the tree. The eleven `false` rows
// are declarable and round-trip fine — they simply have no consumer yet, which is a gap, not a lie.
export const capabilityRows: Record<Lang, CapabilityRow[]> = {
  en: [
    { name: "structured_output_native", type: "bool = false", read: true, reader: "select_output_schema_strategy — picks output_schema_strategy::native (chat_client.hpp:246)" },
    { name: "tool_calling", type: "bool = false", read: true, reader: "select_output_schema_strategy — the tool_shaped fallback when native is unavailable (chat_client.hpp:247)" },
    { name: "reasoning", type: "bool = false", read: true, reader: "BOTH backends' build_request_body — a non-off reasoning_effort without this bit is refused, not dropped (openai:317, anthropic:439)" },
    { name: "prompt_caching", type: "bool = false", read: true, reader: "Anthropic only — switches `system` to the array-of-blocks form and puts cache_control on the LAST tool (anthropic:390, :414)" },
    { name: "max_output_tokens", type: "uint64 = 0", read: true, reader: "Anthropic only — becomes the required max_tokens field, or kDefaultMaxTokens = 4096 when 0; also the base the thinking budget is a percentage of (anthropic:385)" },
    { name: "multimodal_in_image / _audio / _video / _file", type: "bool = false", read: true, reader: "validate_outbound_media_capabilities — refuses a request carrying Media the backend has not declared, rather than letting translate_message drop it silently (chat_client.hpp:275-284)" },
    { name: "streaming", type: "bool = false", read: false, reader: "Nothing. chat_stream() is always available; the bit is declared and never consulted." },
    { name: "parallel_tool_calls", type: "bool = false", read: false, reader: "Nothing. Neither backend emits a parallel_tool_calls field; the streaming decoders correlate parallel calls by wire `index` regardless." },
    { name: "json_mode", type: "bool = false", read: false, reader: "Nothing — notably NOT consulted by select_output_schema_strategy, which only looks at structured_output_native and tool_calling." },
    { name: "reasoning_encrypted", type: "bool = false", read: false, reader: "Nothing. Anthropic's redacted_thinking blocks become Reasoning{encrypted=true} unconditionally." },
    { name: "multimodal_out", type: "bool = false", read: false, reader: "Nothing." },
    { name: "logprobs", type: "bool = false", read: false, reader: "Nothing." },
    { name: "stop_sequences", type: "bool = false", read: false, reader: "Nothing. No backend emits stop sequences — sampling parameters are elided from ChatRequest entirely (chat_client.hpp:81)." },
    { name: "seed", type: "bool = false", read: false, reader: "Nothing reads the BIT. OpenAIChatClient does emit a `seed` body field, but from its own constructor parameter, not from this capability." },
    { name: "token_counting", type: "bool = false", read: false, reader: "Nothing. No count-tokens endpoint is called anywhere." },
    { name: "batch", type: "bool = false", read: false, reader: "Nothing. 004 §8 Q1 resolved batch as first-class; no code implements it." },
    { name: "context_window", type: "uint64 = 0", read: false, reader: "Nothing. No context-window check exists on the request path." },
  ],
  vi: [
    { name: "structured_output_native", type: "bool = false", read: true, reader: "select_output_schema_strategy — chọn output_schema_strategy::native (chat_client.hpp:246)" },
    { name: "tool_calling", type: "bool = false", read: true, reader: "select_output_schema_strategy — phương án tool_shaped khi không có native (chat_client.hpp:247)" },
    { name: "reasoning", type: "bool = false", read: true, reader: "build_request_body của CẢ HAI backend — một reasoning_effort khác off mà thiếu bit này sẽ bị từ chối, không bị bỏ qua (openai:317, anthropic:439)" },
    { name: "prompt_caching", type: "bool = false", read: true, reader: "Chỉ Anthropic — chuyển `system` sang dạng mảng khối và đặt cache_control lên tool CUỐI CÙNG (anthropic:390, :414)" },
    { name: "max_output_tokens", type: "uint64 = 0", read: true, reader: "Chỉ Anthropic — trở thành trường max_tokens bắt buộc, hoặc kDefaultMaxTokens = 4096 khi bằng 0; cũng là cơ số mà ngân sách thinking tính phần trăm trên đó (anthropic:385)" },
    { name: "multimodal_in_image / _audio / _video / _file", type: "bool = false", read: true, reader: "validate_outbound_media_capabilities — từ chối một request mang Media mà backend chưa khai báo, thay vì để translate_message lặng lẽ bỏ nó đi (chat_client.hpp:275-284)" },
    { name: "streaming", type: "bool = false", read: false, reader: "Không gì cả. chat_stream() luôn sẵn có; bit này được khai báo và không bao giờ được tra cứu." },
    { name: "parallel_tool_calls", type: "bool = false", read: false, reader: "Không gì cả. Không backend nào phát ra trường parallel_tool_calls; các bộ giải mã streaming vẫn tương quan các lệnh gọi song song theo `index` trên dây bất kể bit này." },
    { name: "json_mode", type: "bool = false", read: false, reader: "Không gì cả — đáng chú ý là select_output_schema_strategy KHÔNG tra cứu nó, hàm này chỉ nhìn structured_output_native và tool_calling." },
    { name: "reasoning_encrypted", type: "bool = false", read: false, reader: "Không gì cả. Các khối redacted_thinking của Anthropic trở thành Reasoning{encrypted=true} một cách vô điều kiện." },
    { name: "multimodal_out", type: "bool = false", read: false, reader: "Không gì cả." },
    { name: "logprobs", type: "bool = false", read: false, reader: "Không gì cả." },
    { name: "stop_sequences", type: "bool = false", read: false, reader: "Không gì cả. Không backend nào phát ra stop sequence — các tham số sampling bị lược bỏ hoàn toàn khỏi ChatRequest (chat_client.hpp:81)." },
    { name: "seed", type: "bool = false", read: false, reader: "Không gì đọc BIT này. OpenAIChatClient có phát ra trường `seed` trong thân gói tin, nhưng lấy từ tham số constructor của chính nó, không phải từ capability này." },
    { name: "token_counting", type: "bool = false", read: false, reader: "Không gì cả. Không nơi nào gọi endpoint đếm token." },
    { name: "batch", type: "bool = false", read: false, reader: "Không gì cả. 004 §8 Q1 đã kết luận batch là hạng nhất; chưa có mã nào cài đặt." },
    { name: "context_window", type: "uint64 = 0", read: false, reader: "Không gì cả. Không có kiểm tra cửa sổ ngữ cảnh nào trên đường request." },
  ],
};

// ---- reasoning_effort: one ordinal level, two genuinely different native shapes ---------------

export interface ReasoningRow {
  level: string;
  openai: string;
  anthropic: string;
}

export const reasoningRows: Record<Lang, ReasoningRow[]> = {
  en: [
    { level: "nullopt", openai: "no field emitted at all", anthropic: "no field emitted at all" },
    { level: "off", openai: '"reasoning_effort": "none"', anthropic: '"thinking": {"type": "disabled"}' },
    { level: "low", openai: '"reasoning_effort": "low"', anthropic: 'budget_tokens = 25% of max_tokens, floored at 1024' },
    { level: "medium", openai: '"reasoning_effort": "medium"', anthropic: 'budget_tokens = 50% of max_tokens, floored at 1024' },
    { level: "high", openai: '"reasoning_effort": "high"', anthropic: 'budget_tokens = 75% of max_tokens — deliberately not ~100%, so the visible answer still has room' },
  ],
  vi: [
    { level: "nullopt", openai: "không phát ra trường nào cả", anthropic: "không phát ra trường nào cả" },
    { level: "off", openai: '"reasoning_effort": "none"', anthropic: '"thinking": {"type": "disabled"}' },
    { level: "low", openai: '"reasoning_effort": "low"', anthropic: 'budget_tokens = 25% của max_tokens, sàn là 1024' },
    { level: "medium", openai: '"reasoning_effort": "medium"', anthropic: 'budget_tokens = 50% của max_tokens, sàn là 1024' },
    { level: "high", openai: '"reasoning_effort": "high"', anthropic: 'budget_tokens = 75% của max_tokens — cố ý không phải ~100%, để câu trả lời hiển thị vẫn còn chỗ' },
  ],
};

// ---- The conformer inventory -----------------------------------------------------------------

export interface ConformerRow {
  name: string;
  shape: string;
  file: string;
  note: string;
}

export const conformerRows: Record<Lang, ConformerRow[]> = {
  en: [
    {
      name: "OpenAIChatClient<Store>",
      shape: "ChatClient + LegacyChatClient",
      file: "protocol/openai/chat_client.hpp:881",
      note: "POSTs {path_prefix}/chat/completions. Holds host/port/model/SecretRef and a Store const&. capabilities() returns the constructor's caps verbatim — nothing is probed. producer_chat_client_id() reports \"openai:<model>\". Optional constructor extras, all appended never inserted: OpenRouter's HTTP-Referer/X-Title, an end-user id, a seed, the transport enumerator, and ADR-023's operator-armed response-format leak scan (off by default).",
    },
    {
      name: "AnthropicChatClient<Store>",
      shape: "ChatClient + LegacyChatClient",
      file: "protocol/anthropic/chat_client.hpp:1071",
      note: "POSTs {path_prefix}/messages with an anthropic-version header (default 2023-06-01). The only constructor in the set that can throw: an invalid cache_ttl (anything but \"\", \"5m\", \"1h\") is rejected at construction, because result<T> has no return channel from a constructor. No seed parameter — Anthropic has none. producer_chat_client_id() reports \"anthropic:<model>\".",
    },
    {
      name: "ReplayChatClient",
      shape: "ChatClient (static_assert'd in-header)",
      file: "core/replay_chat_client.hpp:122",
      note: "Not a template. Serves a ChatCallRecording; ignores the live request entirely. Mode is checked, not guessed: a streaming recording fed to chat() fails replay_chat_client.mode_mismatch_streaming and the converse fails mode_mismatch_unary.",
    },
    {
      name: "RecordingChatClient<Inner>",
      shape: "ChatClient; gates Inner on LegacyChatClient",
      file: "core/recording_chat_client.hpp:116",
      note: "Owns Inner by value and forwards capabilities() unchanged. Gates on LegacyChatClient rather than ChatClient deliberately — after chat() left the required shape, a chat_stream()-only backend would have compiled here and then failed deep inside .chat() with an opaque template error.",
    },
    {
      name: "ModelCallGateway<Primary, Fallback...>",
      shape: "ModelCallGatewayLike — NOT a ChatClient",
      file: "core/model_call_gateway.hpp:120",
      note: "Retry + circuit breaker + failover in one type, not layered. Fallback... may be empty. capabilities() reports the PRIMARY's only, never a merge. One rt::CircuitBreaker per backend. Defaults: 3 attempts, 100 ms base, 5 s ceiling, 0.2 jitter; breaker opens after 5 failures for 30 s.",
    },
    {
      name: "MiddlewareModelCallGateway<Inner, Ms...>",
      shape: "ModelCallGatewayLike — NOT a ChatClient",
      file: "core/model_call_gateway.hpp:305",
      note: "Hooks only — it wraps another gateway, not a raw backend. A before_model hook may settle the call and skip the backend entirely, and every after_model hook still runs on the way out. enforce_backend_tool_call_provenance() then demotes any ToolCall that did not come verbatim from the real backend to call_provenance::text_derived.",
    },
  ],
  vi: [
    {
      name: "OpenAIChatClient<Store>",
      shape: "ChatClient + LegacyChatClient",
      file: "protocol/openai/chat_client.hpp:881",
      note: "Gửi POST tới {path_prefix}/chat/completions. Giữ host/port/model/SecretRef và một Store const&. capabilities() trả về đúng nguyên văn caps của constructor — không dò tìm gì cả. producer_chat_client_id() báo \"openai:<model>\". Các tham số constructor tùy chọn, luôn thêm vào cuối chứ không chèn giữa: HTTP-Referer/X-Title của OpenRouter, một id người dùng cuối, một seed, enumerator transport, và bộ quét rò rỉ response-format của ADR-023 do người vận hành bật (mặc định tắt).",
    },
    {
      name: "AnthropicChatClient<Store>",
      shape: "ChatClient + LegacyChatClient",
      file: "protocol/anthropic/chat_client.hpp:1071",
      note: "Gửi POST tới {path_prefix}/messages kèm header anthropic-version (mặc định 2023-06-01). Đây là constructor duy nhất trong nhóm có thể ném ngoại lệ: một cache_ttl không hợp lệ (khác \"\", \"5m\", \"1h\") bị từ chối ngay lúc khởi tạo, vì result<T> không có kênh trả về từ một constructor. Không có tham số seed — Anthropic không có khái niệm đó. producer_chat_client_id() báo \"anthropic:<model>\".",
    },
    {
      name: "ReplayChatClient",
      shape: "ChatClient (được static_assert ngay trong header)",
      file: "core/replay_chat_client.hpp:122",
      note: "Không phải template. Phục vụ một ChatCallRecording; hoàn toàn bỏ qua request đang chạy. Chế độ được kiểm tra chứ không đoán: một bản ghi streaming đưa vào chat() sẽ lỗi replay_chat_client.mode_mismatch_streaming, và chiều ngược lại lỗi mode_mismatch_unary.",
    },
    {
      name: "RecordingChatClient<Inner>",
      shape: "ChatClient; ràng buộc Inner theo LegacyChatClient",
      file: "core/recording_chat_client.hpp:116",
      note: "Sở hữu Inner theo giá trị và chuyển tiếp capabilities() nguyên vẹn. Việc ràng buộc theo LegacyChatClient thay vì ChatClient là cố ý — sau khi chat() rời khỏi hình dạng bắt buộc, một backend chỉ có chat_stream() sẽ biên dịch qua ở đây rồi hỏng sâu bên trong .chat() với một lỗi template khó hiểu.",
    },
    {
      name: "ModelCallGateway<Primary, Fallback...>",
      shape: "ModelCallGatewayLike — KHÔNG phải ChatClient",
      file: "core/model_call_gateway.hpp:120",
      note: "Thử lại + circuit breaker + chuyển dự phòng gộp trong một kiểu, không xếp lớp. Fallback... có thể rỗng. capabilities() chỉ báo của backend CHÍNH, không bao giờ hợp nhất. Mỗi backend một rt::CircuitBreaker riêng. Mặc định: 3 lần thử, 100 ms cơ sở, trần 5 s, jitter 0.2; breaker mở sau 5 lần lỗi trong 30 s.",
    },
    {
      name: "MiddlewareModelCallGateway<Inner, Ms...>",
      shape: "ModelCallGatewayLike — KHÔNG phải ChatClient",
      file: "core/model_call_gateway.hpp:305",
      note: "Chỉ có hook — nó bọc một gateway khác, không phải một backend thô. Một hook before_model có thể tự giải quyết lệnh gọi và bỏ qua hẳn backend, và mọi hook after_model vẫn chạy trên đường quay ra. Sau đó enforce_backend_tool_call_provenance() hạ mọi ToolCall không đến nguyên văn từ backend thật xuống call_provenance::text_derived.",
    },
  ],
};

// ---- The credential path, step by step -------------------------------------------------------

export interface CredentialStep {
  index: string;
  title: string;
  body: string;
}

export const credentialSteps: Record<Lang, CredentialStep[]> = {
  en: [
    { index: "1", title: "Construction takes a name", body: "OpenAIChatClient/AnthropicChatClient are constructed with SecretRef{\"OPENAI_API_KEY\"} and a Store const&. SecretRef is a bare struct with one std::string name — what appears in configuration, documents and plugin manifests. Nothing resolvable is stored." },
    { index: "2", title: "chat() begins, and resolves — here, not earlier", body: "store_.resolve(api_key_ref_, ctx) is the first statement in both chat() and chat_stream(). 018 §4's rule is that a native seam backend earns no exemption from \"never read into a config struct at startup\", and this is where that rule is kept." },
    { index: "3", title: "The capability gate fires first, and fails closed", body: "require_secret_capability() checks ctx.capabilities for a granted cap::Secret naming exactly this ref. A null pointer or a missing grant both return failure_class::policy / secret.not_granted before the SecretSource is touched at all. Subsumption is by exact name, with any granted TTL covering the zero-TTL request." },
    { index: "4", title: "The source produces a zeroizing buffer", body: "EnvSecretSource reads an environment variable; FileSecretSource reads <dir>/<name> and strips one trailing newline. Either way the bytes land in a Secret — heap-allocated, wiped in its destructor, with bytes() as its only accessor and no std::string conversion or operator<< to leak through." },
    { index: "5", title: "reveal_text(), once, at the header", body: "SecretLease wraps that Secret with the ref it came from, is non-copyable by static_assert, and exposes exactly one loudly-named accessor. It is called on the line that builds the header: Authorization: Bearer <key> for the OpenAI-compatible backend, x-api-key for Anthropic. Grepping for reveal_text( finds every such point of use in the tree." },
    { index: "6", title: "The lease dies with the call", body: "There is no SecretLease member on either client. When chat() returns, the lease and its Secret go out of scope and the buffer is wiped — so a crash dump or a config dump taken between two calls has nothing to find. On the streaming path the revealed text is passed by value into the detached worker, which touches no state owned by the client at all." },
  ],
  vi: [
    { index: "1", title: "Khởi tạo chỉ nhận một cái tên", body: "OpenAIChatClient/AnthropicChatClient được khởi tạo với SecretRef{\"OPENAI_API_KEY\"} và một Store const&. SecretRef là một struct trần với một std::string name — thứ xuất hiện trong cấu hình, tài liệu và manifest của plugin. Không có gì có thể phân giải được bị lưu lại." },
    { index: "2", title: "chat() bắt đầu, và phân giải — tại đây, không sớm hơn", body: "store_.resolve(api_key_ref_, ctx) là câu lệnh đầu tiên trong cả chat() lẫn chat_stream(). Quy tắc của 018 §4 là một backend seam gốc (native) không được miễn trừ khỏi điều \"không bao giờ đọc vào một struct cấu hình lúc khởi động\", và đây chính là nơi quy tắc đó được giữ." },
    { index: "3", title: "Cổng capability nổ trước tiên, và từ chối đóng", body: "require_secret_capability() kiểm tra ctx.capabilities xem có một cap::Secret được cấp và gọi đúng tên ref này không. Con trỏ rỗng hay thiếu quyền cấp đều trả về failure_class::policy / secret.not_granted trước khi SecretSource bị chạm tới. Việc bao hàm dựa trên tên khớp chính xác, với mọi TTL đã cấp đều bao phủ yêu cầu TTL bằng không." },
    { index: "4", title: "Nguồn tạo ra một bộ đệm tự xóa", body: "EnvSecretSource đọc một biến môi trường; FileSecretSource đọc <dir>/<name> và loại một ký tự xuống dòng ở cuối. Dù bằng cách nào, các byte cũng nằm trong một Secret — cấp phát trên heap, bị xóa sạch trong destructor, chỉ có bytes() là accessor duy nhất, không có chuyển đổi sang std::string và không có operator<< để rò rỉ qua." },
    { index: "5", title: "reveal_text(), một lần, ngay tại header", body: "SecretLease bọc Secret đó cùng ref mà nó đến từ, không thể sao chép nhờ static_assert, và phơi ra đúng một accessor được đặt tên thật rõ. Nó được gọi ngay trên dòng dựng header: Authorization: Bearer <key> cho backend tương thích OpenAI, x-api-key cho Anthropic. Grep \"reveal_text(\" là tìm ra mọi điểm sử dụng như vậy trong cây mã." },
    { index: "6", title: "Lease chết cùng lệnh gọi", body: "Không client nào có thành viên SecretLease. Khi chat() trả về, lease và Secret của nó ra khỏi phạm vi và bộ đệm bị xóa sạch — nên một bản crash dump hay config dump lấy giữa hai lệnh gọi không tìm thấy gì. Trên đường streaming, đoạn văn bản đã lộ được truyền theo giá trị vào worker tách rời, và worker đó hoàn toàn không chạm vào trạng thái nào do client sở hữu." },
  ],
};

// ---- Guest vs host egress: the ADR-016 split --------------------------------------------------

export interface EgressRow {
  aspect: string;
  guest: string;
  host: string;
}

export const egressRows: Record<Lang, EgressRow[]> = {
  en: [
    { aspect: "Entry point", guest: "HostEgressProxy::fetch — a WASM guest asking to reach a host", host: "perform_provider_https_exchange / perform_provider_streaming_exchange — a ChatClient reaching its configured endpoint" },
    { aspect: "Resolver", guest: "resolve_and_validate — blocked-range table enforced", host: "resolve_host — same shared resolution, no blocked-range table" },
    { aspect: "Blocked ranges", guest: "127/8, 169.254/16 (incl. the cloud metadata address), the three RFC 1918 ranges, 100.64/10, 224/4, 240/4, 0/8", host: "None. A local llama.cpp/vLLM/Ollama endpoint IS the ordinary case 004 §3 names, not an SSRF attempt." },
    { aspect: "Why the difference", guest: "The destination comes from a grant a sandboxed guest asked to use, in a system where guest-reachable values may be model-derived. There is a real attacker.", host: "The destination is a field of the ChatClient the deployment constructed from its own config. By I3 it can never come from model output. A check with no attacker to stop is a false positive, not defence in depth." },
    { aspect: "DNS rebinding", guest: "Resolve once, connect to a verified numeric literal — never a hostname", host: "Identical. This is transport- and threat-model-independent and was not relaxed." },
    { aspect: "CRLF header gate", guest: "reject_crlf() on guest-supplied method/path/header fields", host: "None — every field is host-constructed by the backend's own translation, so 006 §7's taint boundary does not reach here." },
    { aspect: "Transport", guest: "https / http per the grant", host: "ProviderTransport::tls by default; plaintext_http only if the construction site names it" },
    { aspect: "Byte cap", guest: "The grant's own byte_cap, clamped to a 16 MiB hard ceiling, enforced DURING the read loop", host: "Same ceiling and same in-loop enforcement" },
    { aspect: "Not claimed", guest: "—", host: "This does not make the provider path safe against a compromised configuration. An operator who can set the provider host can already point it anywhere; that is an 018 operator-trust boundary, not an address table's job." },
  ],
  vi: [
    { aspect: "Điểm vào", guest: "HostEgressProxy::fetch — một guest WASM xin ra tới một host", host: "perform_provider_https_exchange / perform_provider_streaming_exchange — một ChatClient đi tới endpoint đã cấu hình của nó" },
    { aspect: "Resolver", guest: "resolve_and_validate — bảng dải bị chặn được thực thi", host: "resolve_host — cùng một phần phân giải dùng chung, không có bảng dải bị chặn" },
    { aspect: "Các dải bị chặn", guest: "127/8, 169.254/16 (gồm cả địa chỉ metadata của đám mây), ba dải RFC 1918, 100.64/10, 224/4, 240/4, 0/8", host: "Không có. Một endpoint llama.cpp/vLLM/Ollama cục bộ CHÍNH LÀ trường hợp thông thường mà 004 §3 nêu tên, không phải một mưu toan SSRF." },
    { aspect: "Vì sao khác nhau", guest: "Đích đến đến từ một quyền cấp mà một guest bị cô lập xin dùng, trong một hệ thống nơi các giá trị guest chạm tới được có thể bắt nguồn từ model. Có một kẻ tấn công thật.", host: "Đích đến là một trường của chính ChatClient mà deployment dựng lên từ cấu hình của mình. Theo I3, nó không bao giờ có thể đến từ đầu ra của model. Một kiểm tra không có kẻ tấn công nào để chặn là một dương tính giả, không phải phòng thủ theo chiều sâu." },
    { aspect: "DNS rebinding", guest: "Phân giải một lần, kết nối tới một địa chỉ số đã xác minh — không bao giờ tới một hostname", host: "Giống hệt. Cơ chế này độc lập với transport và mô hình mối đe dọa, và đã không được nới lỏng." },
    { aspect: "Cổng chặn CRLF", guest: "reject_crlf() trên các trường method/path/header do guest cung cấp", host: "Không có — mọi trường đều do host dựng qua chính phần dịch của backend, nên ranh giới nhiễm bẩn của 006 §7 không chạm tới đây." },
    { aspect: "Transport", guest: "https / http tùy theo quyền cấp", host: "Mặc định ProviderTransport::tls; plaintext_http chỉ khi nơi khởi tạo gọi đích danh" },
    { aspect: "Trần byte", guest: "byte_cap của chính quyền cấp, kẹp dưới trần cứng 16 MiB, thực thi NGAY TRONG vòng đọc", host: "Cùng trần và cùng cách thực thi ngay trong vòng đọc" },
    { aspect: "Điều không tuyên bố", guest: "—", host: "Điều này không làm cho đường provider an toàn trước một cấu hình đã bị xâm phạm. Người vận hành đặt được host của provider thì vốn đã trỏ nó đi đâu cũng được; đó là ranh giới tin cậy người vận hành thuộc 018, không phải việc của một bảng địa chỉ." },
  ],
};

// ---- Live-endpoint evidence -------------------------------------------------------------------

export interface LiveTestRow {
  file: string;
  gate: string;
  proves: string;
}

export const liveTestRows: Record<Lang, LiveTestRow[]> = {
  en: [
    {
      file: "tests/test_openrouter_live_e2e.cpp",
      gate: "AGENTENGINE_OPENROUTER_API_KEY unset → prints SKIPPED, exits 0. ctest label live-network, TIMEOUT 300. Host/model overridable; tools/run-live-provider-tests.ps1 populates the environment and runs ctest -L live-network.",
      proves: "16 checks across both backends against one host that exposes BOTH wire contracts. OR-OAI-1/ANT-1: chat() completes. OR-OAI-2: data:-framed SSE decodes; OR-ANT-2: named-event SSE decodes. OR-OAI-3/ANT-3: the same ToolDescriptor projects onto two different tool shapes and a real model calls it. OR-OAI-4/ANT-4: the tool RESULT turn is accepted back — the half a canned server cannot test. OR-OAI-5/ANT-5: structured output via response_format and via output_config. OR-ANT-6: system extraction plus prompt-caching breakpoints on a real request. OR-OAI-7/ANT-7 are POSITIVE CONTROLS — a deliberately wrong credential must be rejected by the real service. OR-OAI-8/ANT-8: an ungranted cap::Secret fails closed BEFORE any egress. OR-ANT-8: reasoning_effort has an observable effect. OR-PARITY-1: one unchanged call site, two real backends (004 §7 G1, I6). This is the only test in the suite that exercises the real DNS resolver and the real vendored CA bundle together.",
    },
    {
      file: "tests/test_llamacpp_live_e2e.cpp",
      gate: "AGENTENGINE_LLAMACPP_PORT unset → SKIPPED, exit 0. ctest label live-network, TIMEOUT 300. Host defaults to 127.0.0.1; a prompt prefix is configurable because ChatRequest carries no sampling parameters to turn a thinking model down with.",
      proves: "ADR-016's gate G5 — the endpoint class 004 §3 names explicitly and that motivated the ADR. LC-1: the real client against a real local model, with usage mapped from the server's own counters. LC-2/LC-3: tool calling and the tool-result turn. LC-4: structured output against a grammar-constrained local decoder. LC-5: TLS is still the default — the same endpoint is NOT reachable without opting in. LC-6: chunked SSE streaming genuinely works over the plaintext transport, which falsified an earlier draft of ADR-016 that claimed it did not. LC-7: an ungranted capability fails closed before egress. LC-8: the GUEST path is still refused at that very same live address.",
    },
    {
      file: "tests/test_provider_egress_address_policy.cpp",
      gate: "None — deterministic, offline, a plain-HTTP loopback server, no credential, no egress. Deliberately NOT labelled live-network: it runs in the default suite.",
      proves: "The positive control ADR-016 needed. Relaxing the SSRF table on one path while keeping it on the other is only defensible if BOTH halves are shown against the SAME live loopback address in the SAME run, which is exactly what this does — gates G1-G4.",
    },
    {
      file: "tests/test_openai_chat_client_live.cpp · tests/test_anthropic_chat_client_live.cpp",
      gate: "No environment gate — they run in the default suite over a real TLS socket to a canned loopback server, with an injected resolver and a self-signed leaf.",
      proves: "Plumbing and parsing end to end, including TLS. What they cannot prove, and say so in their own headers, is that what this project SENDS is well-formed: a canned server drains and replies unconditionally where a real one returns HTTP 400. That is the gap the two live-network files above exist to close.",
    },
  ],
  vi: [
    {
      file: "tests/test_openrouter_live_e2e.cpp",
      gate: "Nếu AGENTENGINE_OPENROUTER_API_KEY chưa đặt → in SKIPPED, thoát mã 0. Nhãn ctest live-network, TIMEOUT 300. Host/model có thể ghi đè; tools/run-live-provider-tests.ps1 nạp môi trường và chạy ctest -L live-network.",
      proves: "16 nhóm kiểm tra trên cả hai backend, đối diện một host phơi bày CẢ HAI hợp đồng dây. OR-OAI-1/ANT-1: chat() hoàn tất. OR-OAI-2: SSE đóng khung data: giải mã được; OR-ANT-2: SSE sự-kiện-có-tên giải mã được. OR-OAI-3/ANT-3: cùng một ToolDescriptor chiếu sang hai hình dạng tool khác nhau và một model thật gọi nó. OR-OAI-4/ANT-4: lượt KẾT QUẢ tool được chấp nhận trở lại — nửa mà một máy chủ đóng hộp không kiểm thử được. OR-OAI-5/ANT-5: đầu ra có cấu trúc qua response_format và qua output_config. OR-ANT-6: tách system cùng các điểm ngắt prompt-caching trên một request thật. OR-OAI-7/ANT-7 là ĐỐI CHỨNG DƯƠNG — một credential cố tình sai phải bị dịch vụ thật từ chối. OR-OAI-8/ANT-8: một cap::Secret chưa được cấp sẽ từ chối đóng TRƯỚC mọi lần ra mạng. OR-ANT-8: reasoning_effort có tác dụng quan sát được. OR-PARITY-1: một điểm gọi không đổi, hai backend thật (004 §7 G1, I6). Đây là bài kiểm thử duy nhất trong bộ chạy cả resolver DNS thật lẫn bộ CA đã ghim thật cùng lúc.",
    },
    {
      file: "tests/test_llamacpp_live_e2e.cpp",
      gate: "Nếu AGENTENGINE_LLAMACPP_PORT chưa đặt → SKIPPED, thoát mã 0. Nhãn ctest live-network, TIMEOUT 300. Host mặc định 127.0.0.1; tiền tố prompt cấu hình được, vì ChatRequest không mang tham số sampling nào để hạ bớt một model hay suy luận.",
      proves: "Cổng G5 của ADR-016 — chính lớp endpoint mà 004 §3 nêu đích danh và là động cơ của ADR đó. LC-1: client thật đối diện một model cục bộ thật, với usage ánh xạ từ chính bộ đếm của máy chủ. LC-2/LC-3: gọi tool và lượt trả kết quả tool. LC-4: đầu ra có cấu trúc đối diện một bộ giải mã cục bộ bị ràng buộc bằng ngữ pháp. LC-5: TLS vẫn là mặc định — cùng endpoint đó KHÔNG với tới được nếu không khai báo rõ. LC-6: streaming SSE dạng chunked thực sự chạy được trên transport plaintext, điều này bác bỏ một bản nháp trước đó của ADR-016 vốn khẳng định ngược lại. LC-7: một capability chưa được cấp từ chối đóng trước khi ra mạng. LC-8: đường GUEST vẫn bị từ chối tại đúng địa chỉ đang chạy đó.",
    },
    {
      file: "tests/test_provider_egress_address_policy.cpp",
      gate: "Không có cổng nào — tất định, ngoại tuyến, một máy chủ loopback HTTP thuần, không credential, không ra mạng. Cố ý KHÔNG gắn nhãn live-network: nó chạy trong bộ mặc định.",
      proves: "Đối chứng dương mà ADR-016 cần. Việc nới lỏng bảng SSRF trên một đường trong khi vẫn giữ ở đường kia chỉ biện minh được nếu CẢ HAI nửa được chứng minh trên CÙNG một địa chỉ loopback đang chạy trong CÙNG một lần chạy, và đó đúng là điều bài này làm — các cổng G1-G4.",
    },
    {
      file: "tests/test_openai_chat_client_live.cpp · tests/test_anthropic_chat_client_live.cpp",
      gate: "Không có cổng môi trường — chúng chạy trong bộ mặc định qua một socket TLS thật tới một máy chủ loopback đóng hộp, với resolver được tiêm vào và một chứng chỉ lá tự ký.",
      proves: "Phần đấu nối và phân tích cú pháp từ đầu tới cuối, bao gồm cả TLS. Điều chúng không chứng minh được, và tự nói ra trong header của mình, là những gì dự án này GỬI ĐI có đúng dạng hay không: một máy chủ đóng hộp đọc hết rồi trả lời vô điều kiện, còn máy chủ thật trả về HTTP 400. Đó chính là khoảng trống mà hai tệp live-network ở trên tồn tại để lấp.",
    },
  ],
};

// ---- Honest gaps against 004's own text --------------------------------------------------------

export interface GapRow {
  area: string;
  asked: string;
  built: string;
}

export const gapRows: Record<Lang, GapRow[]> = {
  en: [
    { area: "§3 — Remote agent as ChatClient", asked: "An A2A peer used where a model would be, so \"delegate the whole turn\" is uniform (012).", built: "Nothing. No type in the tree adapts an A2A peer to the ChatClient shape." },
    { area: "§3 — ChatClient as a WASM plugin", asked: "A backend shipped through 009's ae:provider world when its protocol is exotic.", built: "Nothing. The plugin ABI work that exists is the ae:tool world; there is no ae:provider host." },
    { area: "§3 — OpenAI Responses API", asked: "\"OpenAI-compatible (Chat Completions + Responses)\".", built: "Chat Completions only. protocol/openai/chat_client.hpp says so in its own first line." },
    { area: "§2 — Recording the degradation", asked: "The applied fallback \"is recorded in the run trace and metrics\".", built: "select_output_schema_strategy is a pure function that returns a strategy and emits nothing. No trace sink exists yet (needs 016, Milestone 8) — the header names this scoping rather than implying the trace exists." },
    { area: "§2 — Startup failure with no fallback", asked: "\"If no fallback exists, register_agent<A>() fails at startup.\"", built: "Unreachable as specified: parse_and_repair needs no capability bit, so some strategy always exists. chat_client.hpp says this outright instead of manufacturing a failure case, and names check_output_schema_enforceable as where a future bit that removed even that precondition would get a place to fail." },
    { area: "§4 — Idempotency key on the wire", asked: "\"A retried call carries a stable idempotency key so a provider that supports it does not double-charge.\"", built: "Half. ModelCallGateway::call() stamps one key per logical call and reuses it across every retry and every tier. No backend turns it into a request header — neither vendored SDK documents one, checked in source rather than assumed, so the field exists for the moment a verified mechanism does." },
    { area: "§4 — Breaker key", asked: "\"A provider-level breaker trips per {tenant, provider, model, SecretRef}\" — tenant part of the key, not an afterthought.", built: "One breaker per backend POSITION in the gateway's template parameter list. Tenant is not in the key. 004 §5 itself flags this as ADR-track work that has not cleared design→red-team→prove→judge." },
    { area: "§5 — Cost and budget", asked: "Per-run TokenBudget<N>, per-tenant/session/principal ceilings from configuration, and a cost metric keyed {tenant, provider, model, agent, principal}.", built: "Only the first. AgentSession enforces the per-run token budget and fails the run when it is exceeded. Usage::cost_estimate is serialized and deserialized by the recording codec but never written by any backend; there are no pricing tables, no ceilings from configuration, and no metrics surface." },
    { area: "§6 — Recording privacy modes", asked: "Recordings inherit 016's content-capture controls: off by default, with metadata-only and hashed-content modes.", built: "Nothing. chat_recording.hpp has one mode — full content — and no capture-mode field at all. The compensating control that DOES exist is the secret-hygiene canary scan, which plants a canary and then scans checkpoints, recordings and error text for it." },
    { area: "§7 — G4 performance budget", asked: "Engine overhead per model call, excluding network and provider time, within 023's budget at p50/p99.", built: "Nothing. There is no bench directory and no per-model-call overhead measurement in the tree." },
    { area: "§1 — Sampling parameters", asked: "Not asked for — §1 elides them deliberately.", built: "Elided, and named as elided. reasoning_effort is the single carve-out (ADR-020), and even OpenAI's `seed` reaches the wire as a backend constructor parameter rather than portable ChatRequest vocabulary." },
    { area: "ADR-048 — Outbound media", asked: "Not 004's text, but a real consequence: a ChatRequest may carry Media.", built: "Phase 1 only — a fail-closed gate that refuses a request carrying Media the backend has not declared. There is still no wire encoding for Media in either backend's translate_message; Phase 2 is blocked on RFC 019's blob-store seam, which does not exist anywhere in the tree." },
    { area: "§6 — Replay mismatch detection", asked: "Not asked for by name, but assumed by anyone who has used a cassette-style harness.", built: "There is none. ReplayChatClient ignores the live request and ctx entirely and always serves its one constructor-supplied recording — the header says so outright. The only mismatch it detects is call SHAPE: unary versus streaming, each with its own named error. There is no request hash, no prompt comparison and no cassette miss." },
    { area: "§7 G3 — Proven on each half, not across the seam", asked: "\"A recorded streamed run replays offline with identical chunk boundaries.\"", built: "Both halves are proven, separately: the recorder preserves per-chunk elapsed_since_start and the terminal, and the player reproduces exact inter-chunk deltas against an injected clock. No test in the tree records a real call and then replays that same recording — RecordingChatClient and ReplayChatClient are never composed in one file, and every replay test builds its ChatCallRecording by hand." },
    { area: "§6 — What a recording cannot round-trip", asked: "Not 004's text; a consequence of the format.", built: "ChatRequest::tools records only {name, description, args_schema_json, reply_schema_json} — ToolDescriptor::invoke is a std::function closure, runtime state rather than data. There is deliberately no chat_request_from_json at all, because rehydrating one would fabricate an empty invoke for every tool, which is worse than not offering the function; on read-back request.tools is always empty." },
    { area: "§4 vs §1 — Reliability and live streaming are mutually exclusive", asked: "Both, independently: a credit-controlled stream AND retry/failover/breaker.", built: "Each works; not together. ModelCallGateway drains chat_stream() to completion before it can decide whether to retry or fail over, so a gateway-routed round emits no model_delta at all and raises a one-time warning event instead. The header names this as the cost of the design rather than leaving it to be discovered." },
    { area: "§7 G2 — Bounded cancellation", asked: "\"Cancellation mid-stream releases the connection within a bounded time and leaves no orphaned socket or partial state (checked under ASan + a leak gate).\"", built: "The mechanism is real and complete since ADR-017: make_stream shares one std::stop_source, and both backends thread producer.stop_token() into the streaming exchange, which checks it before connecting and at the top of every read-loop iteration. What is missing is the RFC's own verification posture — the bounded-release test proves the common (fast provider) case, and no ASan or leak-gate run is wired to it in tests/CMakeLists.txt." },
    { area: "Build gate — none of this compiles by default", asked: "Not a 004 clause, but the first thing a reader should know.", built: "Both vendor backends, the provider HTTP client and the TLS stack are entirely inside #ifdef AGENTENGINE_WITH_HTTPS, and that CMake option defaults to OFF. On a default configure, none of the translation, streaming, canned-TLS or live tests are even built. The ChatClient concept, the capability set, the registry, the gateways and Recording/ReplayChatClient are all core and always compiled." },
  ],
  vi: [
    { area: "§3 — Agent ở xa dùng như ChatClient", asked: "Một peer A2A được dùng ở chỗ đáng lẽ là một model, để \"ủy thác trọn một lượt\" trở nên đồng nhất (012).", built: "Chưa có gì. Không kiểu nào trong cây mã chuyển một peer A2A sang hình dạng ChatClient." },
    { area: "§3 — ChatClient dưới dạng plugin WASM", asked: "Một backend phát hành qua world ae:provider của 009 khi giao thức của nó khác lạ.", built: "Chưa có gì. Phần ABI plugin đang tồn tại là world ae:tool; không có host ae:provider nào." },
    { area: "§3 — Responses API của OpenAI", asked: "\"Tương thích OpenAI (Chat Completions + Responses)\".", built: "Chỉ Chat Completions. protocol/openai/chat_client.hpp nói vậy ngay dòng đầu tiên của chính nó." },
    { area: "§2 — Ghi lại sự suy giảm", asked: "Phương án dự phòng được áp dụng \"được ghi vào run trace và metrics\".", built: "select_output_schema_strategy là một hàm thuần túy trả về một chiến lược và không phát ra gì. Chưa có trace sink nào tồn tại (cần 016, Milestone 8) — header nói rõ phạm vi này thay vì ngụ ý rằng trace đã có." },
    { area: "§2 — Thất bại lúc khởi động khi không có fallback", asked: "\"Nếu không có fallback nào, register_agent<A>() thất bại ngay lúc khởi động.\"", built: "Không thể chạm tới đúng như đặc tả: parse_and_repair không cần bit capability nào, nên luôn luôn tồn tại một chiến lược. chat_client.hpp nói thẳng điều này thay vì chế ra một trường hợp lỗi, và chỉ ra check_output_schema_enforceable là nơi mà một bit tương lai xóa bỏ cả tiền đề đó sẽ có chỗ để thất bại." },
    { area: "§4 — Khóa idempotency trên dây", asked: "\"Một lệnh gọi được thử lại mang theo một khóa idempotency ổn định để nhà cung cấp nào hỗ trợ thì không tính phí hai lần.\"", built: "Được một nửa. ModelCallGateway::call() đóng dấu một khóa cho mỗi lệnh gọi logic và tái dùng nó qua mọi lần thử lại và mọi tầng dự phòng. Không backend nào biến nó thành một header của request — cả hai SDK được vendor đều không tài liệu hóa một header như vậy, điều này được kiểm tra trong mã nguồn chứ không phỏng đoán, nên trường này tồn tại sẵn cho thời điểm có một cơ chế đã xác minh." },
    { area: "§4 — Khóa của circuit breaker", asked: "\"Một breaker cấp nhà cung cấp bật theo {tenant, provider, model, SecretRef}\" — tenant là một phần của khóa, không phải chuyện nghĩ sau.", built: "Mỗi VỊ TRÍ backend trong danh sách tham số template của gateway một breaker. Tenant không nằm trong khóa. Chính 004 §5 đánh dấu đây là công việc thuộc diện ADR chưa qua vòng thiết kế→red-team→chứng minh→phán xử." },
    { area: "§5 — Chi phí và ngân sách", asked: "TokenBudget<N> theo từng run, các trần theo tenant/session/principal lấy từ cấu hình, và một chỉ số chi phí khóa theo {tenant, provider, model, agent, principal}.", built: "Chỉ có cái đầu tiên. AgentSession thực thi ngân sách token theo từng run và làm hỏng run khi vượt. Usage::cost_estimate được bộ mã hóa bản ghi tuần tự hóa rồi giải tuần tự hóa nhưng không backend nào từng ghi vào; không có bảng giá, không có trần từ cấu hình, và không có bề mặt metrics." },
    { area: "§6 — Chế độ riêng tư cho bản ghi", asked: "Bản ghi kế thừa các kiểm soát thu thập nội dung của 016: mặc định tắt, có chế độ chỉ-siêu-dữ-liệu và chế độ nội-dung-băm.", built: "Chưa có gì. chat_recording.hpp chỉ có một chế độ — nội dung đầy đủ — và hoàn toàn không có trường chế độ thu thập. Biện pháp bù trừ CÓ tồn tại là bài quét canary vệ sinh bí mật: cắm một canary rồi quét các checkpoint, bản ghi và văn bản lỗi để tìm nó." },
    { area: "§7 — Ngân sách hiệu năng G4", asked: "Chi phí phụ trội của engine cho mỗi lệnh gọi model, không kể thời gian mạng và thời gian của nhà cung cấp, nằm trong ngân sách của 023 ở p50/p99.", built: "Chưa có gì. Không có thư mục bench và không có phép đo chi phí phụ trội theo mỗi lệnh gọi model nào trong cây mã." },
    { area: "§1 — Tham số sampling", asked: "Không được yêu cầu — §1 cố ý lược bỏ chúng.", built: "Đã lược bỏ, và được nêu rõ là đã lược bỏ. reasoning_effort là ngoại lệ duy nhất được tách ra (ADR-020), và ngay cả `seed` của OpenAI cũng ra tới dây qua một tham số constructor của backend chứ không phải qua từ vựng ChatRequest có tính di động." },
    { area: "ADR-048 — Media chiều ra", asked: "Không phải văn bản của 004, nhưng là một hệ quả thật: một ChatRequest có thể mang Media.", built: "Chỉ Phase 1 — một cổng từ chối đóng, khước từ mọi request mang Media mà backend chưa khai báo. Vẫn chưa có mã hóa lên dây cho Media trong translate_message của cả hai backend; Phase 2 bị chặn bởi ranh giới blob-store của RFC 019, thứ không tồn tại ở bất kỳ đâu trong cây mã." },
    { area: "§6 — Phát hiện sai khớp khi phát lại", asked: "Không được nêu đích danh, nhưng là điều bất kỳ ai từng dùng một bộ khung kiểu cassette đều mặc định là có.", built: "Không có. ReplayChatClient hoàn toàn bỏ qua request và ctx đang chạy, và luôn phục vụ đúng một bản ghi do constructor cung cấp — header nói thẳng như vậy. Sai khớp duy nhất nó phát hiện là HÌNH DẠNG lệnh gọi: unary hay streaming, mỗi bên một mã lỗi riêng. Không có băm request, không so sánh prompt, và không có khái niệm 'trượt cassette'." },
    { area: "§7 G3 — Chứng minh trên từng nửa, chưa qua ranh giới", asked: "\"Một run streaming đã ghi phát lại được ngoại tuyến với biên chunk giống hệt.\"", built: "Cả hai nửa đều được chứng minh, nhưng tách rời: bộ ghi giữ elapsed_since_start theo từng chunk cùng trạng thái kết thúc, và bộ phát lại tái tạo chính xác khoảng chênh giữa các chunk dựa trên một đồng hồ được tiêm vào. Không bài kiểm thử nào trong cây mã ghi một lệnh gọi thật rồi phát lại chính bản ghi đó — RecordingChatClient và ReplayChatClient chưa bao giờ được ghép trong cùng một tệp, và mọi bài kiểm thử phát lại đều tự tay dựng ChatCallRecording." },
    { area: "§6 — Điều một bản ghi không mang trọn vẹn được", asked: "Không phải văn bản của 004; là hệ quả của định dạng.", built: "ChatRequest::tools chỉ ghi {name, description, args_schema_json, reply_schema_json} — ToolDescriptor::invoke là một closure std::function, tức trạng thái lúc chạy chứ không phải dữ liệu. Cố ý hoàn toàn không có chat_request_from_json, vì tái dựng một cái sẽ bịa ra một invoke rỗng cho mọi tool, điều còn tệ hơn là không cung cấp hàm đó; khi đọc lại, request.tools luôn rỗng." },
    { area: "§4 đối lại §1 — Độ tin cậy và streaming trực tiếp loại trừ lẫn nhau", asked: "Cả hai, một cách độc lập: một luồng có kiểm soát tín dụng VÀ thử lại/dự phòng/breaker.", built: "Từng cái đều chạy; nhưng không cùng lúc. ModelCallGateway phải rút cạn chat_stream() cho tới hết mới quyết định được nên thử lại hay chuyển dự phòng, nên một vòng đi qua gateway hoàn toàn không phát ra model_delta và thay vào đó nâng một sự kiện warning một lần. Header nói rõ đây là cái giá của thiết kế thay vì để người dùng tự phát hiện." },
    { area: "§7 G2 — Hủy có giới hạn", asked: "\"Hủy giữa luồng giải phóng kết nối trong thời gian có giới hạn và không để lại socket mồ côi hay trạng thái dở dang (kiểm tra dưới ASan cùng một cổng rò rỉ bộ nhớ).\"", built: "Cơ chế là thật và đã đầy đủ kể từ ADR-017: make_stream chia sẻ một std::stop_source duy nhất, và cả hai backend đều luồn producer.stop_token() vào lệnh trao đổi streaming, nơi token được kiểm tra trước khi kết nối và ở đầu mỗi vòng lặp đọc. Thứ còn thiếu là tư thế kiểm chứng mà chính RFC đòi hỏi — bài kiểm thử giải phóng có giới hạn chứng minh trường hợp thông thường (nhà cung cấp phản hồi nhanh), và không có lần chạy ASan hay cổng rò rỉ nào được đấu vào nó trong tests/CMakeLists.txt." },
    { area: "Cổng biên dịch — mặc định không có gì trong số này được biên dịch", asked: "Không phải một mệnh đề của 004, nhưng là điều đầu tiên người đọc nên biết.", built: "Cả hai backend nhà cung cấp, client HTTP cho provider và toàn bộ tầng TLS đều nằm gọn trong #ifdef AGENTENGINE_WITH_HTTPS, và tùy chọn CMake đó mặc định TẮT. Với một lần configure mặc định, không bài kiểm thử dịch, streaming, TLS đóng hộp hay live nào được biên dịch. Concept ChatClient, bộ capability, registry, các gateway cùng Recording/ReplayChatClient đều thuộc lõi và luôn được biên dịch." },
  ],
};

// ---- Governing ADRs ---------------------------------------------------------------------------

export interface AdrRow {
  id: string;
  title: string;
  governs: string;
  file: string;
}

export const adrRows: Record<Lang, AdrRow[]> = {
  en: [
    { id: "ADR-013", title: "HTTPS egress TLS client", governs: "mbedTLS vendored and hash-pinned behind AGENTENGINE_WITH_HTTPS, a separately-pinned CA bundle compiled in at configure time, TLS 1.2 floor, real hostname verification. Platform-native TLS was rejected; so was reusing Quark's SecureTransport, which deliberately disables hostname verification for cluster mTLS.", file: "decisions/ADR-013-https-egress-tls-client.md" },
    { id: "ADR-016", title: "Host-initiated provider egress: address policy and transport", governs: "The guest/host resolver split, and ProviderTransport{tls, plaintext_http} as an opt-in named enumerator. Supersedes the earlier Phase C note that reused resolve_and_validate on the provider path \"as defense-in-depth even though the threat model differs\".", file: "decisions/ADR-016-provider-egress-address-policy.md" },
    { id: "ADR-017", title: "Propagating consumer cancellation into a stream producer's blocking I/O", governs: "Why both backends read producer.stop_token() BEFORE moving the producer into the worker thread — argument evaluation order is unspecified, and a token read after the move never fires. Satisfies 004 §7 G2.", file: "decisions/ADR-017-stream-consumer-cancellation-signal.md" },
    { id: "ADR-019", title: "Incremental streaming of a provider response", governs: "perform_provider_streaming_exchange and its on_body callback — bytes decoded and pushed as they arrive, replacing fetch-everything-then-replay. Satisfies 004 §7 G3.", file: "decisions/ADR-019-incremental-streaming-response.md" },
    { id: "ADR-020", title: "A portable reasoning_effort, and why prompt_cache_key stays deferred", governs: "The one sampling-adjacent knob carved out of §1's elision, as an ordinal level each backend maps down. Also the source of Anthropic's enforce-the-vendor-floor-here rule, after OpenRouter was measured returning HTTP 200 for two request shapes api.anthropic.com rejects.", file: "decisions/ADR-020-portable-reasoning-effort.md" },
    { id: "ADR-035", title: "Every ChatClient conformer becomes genuinely streaming-capable, then chat() stops being required", governs: "chat() leaving the ChatClient concept, LegacyChatClient existing at all, and the text_delta-index hardening in the Anthropic accumulator.", file: "decisions/ADR-035-chatclient-streaming-completeness.md" },
    { id: "ADR-036", title: "A coroutine-based model-call gateway replaces chat_stream() wrapper-parity", governs: "ModelCallGateway and MiddlewareModelCallGateway — and the deletion of FailoverChatClient, ResilientChatClient and MiddlewareChatClient. Giving those three chat_stream() parity had a confirmed use-after-free hazard.", file: "decisions/ADR-036-model-call-gateway.md" },
    { id: "ADR-048", title: "Outbound media capability gate", governs: "validate_outbound_media_capabilities — refusing rather than silently dropping Media, including Media nested inside a ToolResult's own content.", file: "decisions/ADR-048-outbound-media-capability-gate.md" },
    { id: "— none —", title: "Secret resolution for providers", governs: "There is no ADR. The governing documents are 018 §4 (point-of-use resolution), 020 §4 (the SecretSource seam) and 004 §1 — with ADR-009 supplying the capability mechanism the cap::Secret gate relies on.", file: "018-Identity-Authorization-and-Secrets.md" },
    { id: "— none —", title: "Recording and replay", governs: "Also no ADR. 004 §6 plus Milestone 5 Phase G1 govern the shared JSON envelope; ADR-035 and ADR-037 only touched it consequentially, to keep it working across the streaming and runtime migrations.", file: "include/agentengine/core/chat_recording.hpp" },
  ],
  vi: [
    { id: "ADR-013", title: "HTTPS egress TLS client", governs: "mbedTLS được vendor và ghim theo hash sau AGENTENGINE_WITH_HTTPS, một bộ CA ghim riêng biên dịch sẵn lúc configure, sàn TLS 1.2, xác minh hostname thật. TLS gốc của nền tảng đã bị bác bỏ; việc tái dùng SecureTransport của Quark cũng vậy, vì nó cố ý tắt xác minh hostname cho mTLS trong cụm.", file: "decisions/ADR-013-https-egress-tls-client.md" },
    { id: "ADR-016", title: "Host-initiated provider egress: address policy and transport", governs: "Việc tách resolver guest/host, và ProviderTransport{tls, plaintext_http} như một enumerator có tên phải khai báo rõ mới dùng. Thay thế ghi chú Phase C trước đó vốn tái dùng resolve_and_validate trên đường provider \"như phòng thủ theo chiều sâu dù mô hình mối đe dọa khác nhau\".", file: "decisions/ADR-016-provider-egress-address-policy.md" },
    { id: "ADR-017", title: "Propagating consumer cancellation into a stream producer's blocking I/O", governs: "Lý do cả hai backend đọc producer.stop_token() TRƯỚC khi chuyển producer vào luồng worker — thứ tự lượng giá đối số là không xác định, và một token đọc sau khi đã move sẽ không bao giờ kích hoạt. Thỏa mãn 004 §7 G2.", file: "decisions/ADR-017-stream-consumer-cancellation-signal.md" },
    { id: "ADR-019", title: "Incremental streaming of a provider response", governs: "perform_provider_streaming_exchange và callback on_body của nó — các byte được giải mã và đẩy đi ngay khi tới, thay cho lấy-hết-rồi-phát-lại. Thỏa mãn 004 §7 G3.", file: "decisions/ADR-019-incremental-streaming-response.md" },
    { id: "ADR-020", title: "A portable reasoning_effort, and why prompt_cache_key stays deferred", governs: "Núm điều chỉnh cận-sampling duy nhất được tách khỏi phần lược bỏ của §1, dưới dạng một mức thứ tự mà mỗi backend tự ánh xạ xuống. Cũng là nguồn của quy tắc tự-thực-thi-sàn-của-nhà-cung-cấp-ngay-tại-đây phía Anthropic, sau khi đo được OpenRouter trả HTTP 200 cho hai dạng request mà api.anthropic.com từ chối.", file: "decisions/ADR-020-portable-reasoning-effort.md" },
    { id: "ADR-035", title: "Every ChatClient conformer becomes genuinely streaming-capable, then chat() stops being required", governs: "Việc chat() rời khỏi concept ChatClient, sự tồn tại của LegacyChatClient, và phần gia cố chỉ-số-text_delta trong bộ tích lũy phía Anthropic.", file: "decisions/ADR-035-chatclient-streaming-completeness.md" },
    { id: "ADR-036", title: "A coroutine-based model-call gateway replaces chat_stream() wrapper-parity", governs: "ModelCallGateway và MiddlewareModelCallGateway — cùng việc xóa bỏ FailoverChatClient, ResilientChatClient và MiddlewareChatClient. Việc cho ba wrapper đó ngang bằng chat_stream() đã bị xác nhận có nguy cơ use-after-free.", file: "decisions/ADR-036-model-call-gateway.md" },
    { id: "ADR-048", title: "Outbound media capability gate", governs: "validate_outbound_media_capabilities — từ chối thay vì lặng lẽ bỏ Media, kể cả Media lồng bên trong phần content của một ToolResult.", file: "decisions/ADR-048-outbound-media-capability-gate.md" },
    { id: "— không có —", title: "Phân giải bí mật cho nhà cung cấp", governs: "Không có ADR nào. Các tài liệu chi phối là 018 §4 (phân giải tại điểm sử dụng), 020 §4 (ranh giới SecretSource) và 004 §1 — với ADR-009 cung cấp cơ chế capability mà cổng cap::Secret dựa vào.", file: "018-Identity-Authorization-and-Secrets.md" },
    { id: "— không có —", title: "Ghi và phát lại", governs: "Cũng không có ADR. 004 §6 cùng Milestone 5 Phase G1 chi phối phong bì JSON dùng chung; ADR-035 và ADR-037 chỉ chạm vào nó như hệ quả, để giữ nó hoạt động qua các đợt di trú streaming và runtime.", file: "include/agentengine/core/chat_recording.hpp" },
  ],
};

// ---- Code snippets (identical in both languages) ------------------------------------------------

export const chatRequestSnippet = `// core/chat_client.hpp:70-100 -- ONE portable request. Nothing vendor-shaped is in it:
// no max_tokens, no tool_choice, no temperature, no cache_control.
ChatRequest request;
request.messages = {
    Message{role::system, {ContentItem{Text{"You are terse."}}},    "m0"},
    Message{role::user,   {ContentItem{Text{"Weather in Hanoi?"}}}, "m1"},
};
request.tools = {get_weather};                    // 006's real ToolDescriptor -- not a second,
                                                   // provider-facing declaration shape
request.output_schema_json = R"({"type":"object","properties":{"c":{"type":"string"}}})";
request.reasoning_effort = reasoning_effort::medium;   // ADR-020's ordinal level, not a vendor field

// The capability set is DECLARED by the deployment (004 §2/§3), never probed from a response.
ChatClientCapabilities caps;
caps.reasoning         = true;
caps.prompt_caching    = true;
caps.max_output_tokens = 8192;`;

export const openaiBodySnippet = `// openai::detail::build_request_body(request, model, /*stream=*/false, "", nullopt, caps)
// protocol/openai/chat_client.hpp:264-329 -- emitted in exactly this order.
{
  "model": "gpt-5",
  "messages": [
    {"role": "system", "content": "You are terse."},
    {"role": "user",   "content": "Weather in Hanoi?"}
  ],
  "tools": [
    {"type": "function",
     "function": {"name": "get_weather", "description": "...",
                  "parameters": { /* args_schema_json, parsed and passed through raw */ }}}
  ],
  "response_format": {
    "type": "json_schema",
    "json_schema": {"name": "response", "strict": true,
                    "schema": { /* ..., "additionalProperties": false FORCED IN (:218-222) */ }}
  },
  "reasoning_effort": "medium"
}
// NOT emitted by this backend, at all: max_tokens, max_completion_tokens, tool_choice,
// parallel_tool_calls, any sampling parameter, any cache_control. caps.prompt_caching and
// caps.max_output_tokens are never read on this path.
// Headers (:335-349): Content-Type: application/json
//                     Authorization: Bearer <lease->reveal_text()>`;

export const anthropicBodySnippet = `// anthropic::detail::build_request_body(request, model, caps, /*stream=*/false, "", cache_ttl)
// protocol/anthropic/chat_client.hpp:376-461 -- same input, a genuinely different body.
{
  "model": "claude-opus-5",
  "max_tokens": 8192,               // REQUIRED here. caps.max_output_tokens, or kDefaultMaxTokens
                                    // = 4096 when it is 0 (:384-386)
  "system": [{"type": "text", "text": "You are terse.",
              "cache_control": {"type": "ephemeral"}}],
                                    // the ARRAY form only when caps.prompt_caching; otherwise a
                                    // plain string (:390-401). role::system never reaches messages.
  "messages": [
    {"role": "user", "content": [{"type": "text", "text": "Weather in Hanoi?"}]}
  ],
  "tools": [
    {"name": "get_weather", "description": "...",     // FLAT -- no "function" wrapper
     "input_schema": { /* ... */ },
     "cache_control": {"type": "ephemeral"}}          // on the LAST tool only (:412-418)
  ],
  "output_config": {"format": {"type": "json_schema", "schema": { /* ... */ }}},
                                    // no additionalProperties forcing -- this backend does not
                                    // assert an unconfirmed requirement onto the schema
  "thinking": {"type": "enabled", "budget_tokens": 4050}
                                    // medium = (8192 / 100) * 50, integer math, floored at 1024
}
// Rejected BEFORE any socket opens: more than 4 cache_control blocks anywhere in the assembled
// body -> anthropic.cache_control_limit_exceeded (:454-459), because Anthropic answers HTTP 400.
// Headers (:466-482): x-api-key: <lease->reveal_text()> + anthropic-version: 2023-06-01`;

export const openaiSseSnippet = `// What the socket delivers. Only "data:" lines are read -- event:/id:/comment lines are
// ignored, and every event fits on one line. openai::detail::split_sse_data_events (:565-581)
data: {"choices":[{"delta":{"content":"Ha"}}]}

data: {"choices":[{"delta":{"content":"noi"}}]}

data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",
      "function":{"name":"get_weather","arguments":"{\\"loc"}}]}}]}

data: {"choices":[{"delta":{"tool_calls":[{"index":0,
      "function":{"arguments":"ation\\":\\"Hanoi\\"}"}}]}}]}

data: {"choices":[],"usage":{"prompt_tokens":41,"completion_tokens":9}}

data: [DONE]

// -> two Text updates, 1:1 with the vendor's own chunks (items_from_block, :720-726)
// -> ONE ToolCall update: fragments accumulate per "index" and are emitted only at finish()
//    (:646-656), because a partial JSON string is not a valid arguments_json on its own
// -> the trailing usage chunk has choices:[] and is captured BEFORE the empty-choices skip
//    (:708-715). It exists only because build_request_body sent stream_options.include_usage
//    (:296-305); without it the vendor's stream carries no usage at all, and 004 §5's
//    TokenBudget<N> must treat a missing usage as a hard failure, never as zero cost.`;

export const anthropicSseSnippet = `// The same conversation, Anthropic's NAMED-event framing: an "event:" line PAIRED with the
// data: line after it. anthropic::detail::split_sse_named_events (:722-743)
event: message_start
data: {"message":{"usage":{"input_tokens":41,"output_tokens":1}}}

event: content_block_start
data: {"index":0,"content_block":{"type":"text"}}

event: content_block_delta
data: {"index":0,"delta":{"type":"text_delta","text":"Ha"}}

event: content_block_start
data: {"index":1,"content_block":{"type":"tool_use","id":"toolu_1","name":"get_weather"}}

event: content_block_delta
data: {"index":1,"delta":{"type":"input_json_delta","partial_json":"{\\"location\\""}}

event: content_block_stop
data: {"index":1}

event: message_delta
data: {"usage":{"output_tokens":9}}

// -> a text_delta becomes a Text item ONLY if its own index was started as a "text" block
//    (ADR-035 hardening, :907-915): a non-compliant Anthropic-wire gateway -- Bedrock, Vertex,
//    a self-hosted proxy -- must not be able to inject a stray Text through a mismatched kind
// -> input_json_delta / thinking_delta accrue per index; the ToolCall and the Reasoning are
//    emitted whole at finish() (:794-821)
// -> message_delta.usage is CUMULATIVE on this wire: output_tokens is OVERWRITTEN by each
//    event, never summed (accumulate_message_delta_usage, :628-638)
// -> signature_delta is intentionally dropped; redacted_thinking becomes
//    Reasoning{text:"", encrypted:true} rather than a fabricated trace`;

export const credentialSnippet = `// The backend holds a NAME. protocol/openai/chat_client.hpp:996 -- the only credential-shaped
// member is a SecretRef; there is no SecretLease member anywhere in either client.
OpenAIChatClient<AgentEngineSecretStore> client{
    "api.openai.com", 443, "gpt-5",
    SecretRef{"OPENAI_API_KEY"},        // trust/secret.hpp:196-198 -- a name, not a value
    caps, store};

task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext& ctx) const {
    // Resolution happens HERE, inside chat(), against EffectContext -- never at construction
    // (004 §1 / 018 §4). :938-941
    auto lease = store_.resolve(api_key_ref_, ctx);
    if (!lease) co_return std::unexpected(lease.error());
    ...
    auto req = detail::build_http_request(path, lease->reveal_text(), json::dump(*body), ...);
}                                        // :947 -- the one point of use

// ...and resolve() fails CLOSED before it reads anything. trust/secret.hpp:245-255
if (ctx.capabilities == nullptr ||
    !ctx.capabilities->contains(cap::Secret{ref.name, std::chrono::seconds{0}})) {
    return std::unexpected(error{failure_class::policy,
        "secret '" + ref.name + "' resolved without a granted Secret<name> capability",
        "secret.not_granted"});
}
// A null capability pointer is a DENIAL, not a permissive default. Secret zeroizes in its
// destructor, has no std::string conversion and no operator<<; SecretLease is non-copyable by
// static_assert -- so reveal_text( is greppable, and it is the only way out.`;

export const gatewaySnippet = `// core/model_call_gateway.hpp -- ADR-036. NOT a ChatClient: no chat(), no chat_stream(),
// only call(), which must be a real coroutine so a middleware hook can be co_awaited.
using Gateway = ModelCallGateway<AnthropicChatClient<Store>, OpenAIChatClient<Store>>;
Gateway gw{std::move(primary), std::tuple{std::move(fallback)},
           RetryPolicy{},        // 3 attempts, 100 ms base, 5 s ceiling, 0.2 jitter (:33-44)
           BreakerConfig{}};     // opens after 5 failures, stays open 30 s (:48-51)

// capabilities() reports the PRIMARY's only -- never merged, never intersected (:151-154).
// One rt::CircuitBreaker per backend: 1 + sizeof...(Fallback) of them (:143-148).
// try_tier<Tier>: declaration order, first success wins; on success
//   attempt->fallback_tier = Tier, OVERWRITING whatever the backend left there (:179, :192).
// Admit::Shed -> transient/gateway.circuit_open immediately, WITHOUT consuming an attempt
//   slot and without spin-waiting (:214-218).
// A retry fires only on failure_class::transient (:238) and never sleeps past ctx.deadline
//   (:244-255). A Closed terminal carrying NO usage is forced to a failure --
//   gateway.usage_unavailable (:225-229) -- so a budgeted run never counts a call as free.
// Live residual, named in the header rather than hidden: the backoff is a BLOCKING
//   std::this_thread::sleep_for (:270).`;

export const replaySnippet = `// core/recording_chat_client.hpp -- gates Inner on LegacyChatClient (:121-122), because its
// body genuinely calls inner_.chat() and plain ChatClient no longer guarantees one.
RecordingChatClient<AnthropicChatClient<Store>> rec{
    std::move(live),
    [](ChatCallRecording r) { write_chat_call_recording(r, path); }};   // the sink is the CALLER's
// capabilities() is a pure forward (:133). Both success and failure are recorded (:149-157),
// and the inner outcome is returned completely unchanged (:160) -- recording is not a policy.
// Streaming: each chunk is stamped with its own elapsed_since_start and re-pushed in the
// inner's order; recording continues even after the consumer drops the stream, and the sink
// runs BEFORE the producer's terminal is latched (:228-232).

// core/replay_chat_client.hpp -- not a template; static_assert(ChatClient<...>) at :199-202.
ReplayChatClient replay{load_chat_call_recording(path), caps};
// capabilities() are DECLARED by the caller, never inferred from the recording (:134-135).
// run_replay_worker sleeps the delta between successive RecordedChunk::elapsed_since_start
//   before each push (:93-114) -- 004 §7 G3's "identical chunk boundaries" made true in TIME,
//   not merely in delivery order.
// A unary recording fed to chat_stream() fails replay_chat_client.mode_mismatch_unary
//   (:175-179); the converse fails mode_mismatch_streaming (:142-149). Mode is checked, never
//   guessed -- and a recording with neither a response nor an error is
//   replay_chat_client.malformed_recording, not an empty success.`;
