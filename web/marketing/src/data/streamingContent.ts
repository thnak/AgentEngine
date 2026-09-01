// Content for the Streaming detail page (api/streaming.html) — 013 §1 (UI and Streaming Surfaces),
// 004 §1 (ChatClient's chat_stream() signature), ADR-034 (AgentSession's opt-in streaming turn
// loop) and ADR-035 Phase 3 (chat_stream() promoted to the ChatClient concept's only required
// method). Same rule as apiContent.ts: every claim here is grounded in a real header, source file,
// RFC section, ADR, or test/example in THIS repo, with a citation — don't invent a shape or a
// claim these sources don't make, and never blur "real and tested" with "an RFC describes this but
// nothing implements it yet." This page absorbs and expands the streaming material that used to
// live entirely on providers.html (see providerContent.ts's own "streaming" section, kept there
// now only as a short pointer back to this page) — the two SSE-framing snippets below are close
// variants of providerContent.ts's openaiSseSnippet/anthropicSseSnippet, trimmed further since wire
// framing is this page's "if you're curious" appendix, not its main subject.
//
// Bilingual (EN/VI): every translatable field below is a Record<Lang, T>. Code snippets,
// identifiers, RFC/ADR numbers, file paths, and proper nouns stay identical in both languages —
// only surrounding prose is translated.

import { gh, type ApiEntry } from "./apiContent";
import type { Lang } from "../i18n/LanguageContext";

export interface StreamingSection {
  id: string;
  label: string;
}

// Ids match the `id` attributes of the <section>/section-head anchors ApiStreamingReference
// renders — the right-rail scroll-spy TOC reads this list.
export const streamingSections: Record<Lang, StreamingSection[]> = {
  en: [
    { id: "seam", label: "chat_stream() is required" },
    { id: "stream-primitive", label: "agentengine::stream<T>" },
    { id: "response-update", label: "ChatResponseUpdate" },
    { id: "session-streaming", label: "Session-level streaming" },
    { id: "wire", label: "Two SSE wire framings" },
    { id: "worked-example", label: "Worked example" },
  ],
  vi: [
    { id: "seam", label: "chat_stream() là bắt buộc" },
    { id: "stream-primitive", label: "agentengine::stream<T>" },
    { id: "response-update", label: "ChatResponseUpdate" },
    { id: "session-streaming", label: "Streaming ở cấp session" },
    { id: "wire", label: "Hai kiểu đóng khung SSE trên dây" },
    { id: "worked-example", label: "Ví dụ minh họa" },
  ],
};

// ------------------------------------------------------------------------------------------------
// Doc entries (the `doc-entry` list: tag + status badge + title + body + one citation)
// ------------------------------------------------------------------------------------------------

export const streamingEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "entry-chat-client-concept",
      status: "real",
      tag: "concept ChatClient",
      title: "Two required expressions, and chat_stream() is one of them",
      body:
        "Since ADR-035 Phase 3, the concept requires exactly capabilities() and chat_stream(request, ctx) -> stream<ChatResponseUpdate>; the whole-response chat() is deliberately no longer part of the required shape. This WIDENS what can conform rather than narrowing it — every real backend (OpenAIChatClient, AnthropicChatClient) still has a chat() and keeps it, and the relaxation is what let a hypothetical chat_stream()-only backend compile against ChatClient for the first time. A pre-existing sibling, LegacyChatClient, still requires both methods — RecordingChatClient gates on it specifically because its own body unconditionally calls .chat(), and gating on the relaxed ChatClient would let a chat_stream()-only conformer compile there and then fail deep inside .chat() with an opaque template error instead of a named one.",
      cite: "include/agentengine/core/chat_client.hpp:205-209",
      href: gh("include/agentengine/core/chat_client.hpp"),
    },
    {
      id: "entry-stream-type",
      status: "real",
      tag: "class stream<T>",
      title: "A move-only drain handle over a real channel, not a coroutine",
      body:
        "stream<T> wraps rt::channel_consumer<T, error>. next() calls the channel's try_pop() directly — std::optional<T>, nullopt meaning \"nothing buffered right now, not done,\" never a signal to stop draining. done()/terminal()/fail_error() read the channel's own terminal state; gap_detected() is hardcoded false because rt::channel<T,E> structurally cannot drop or dedup an item. Dropping a stream<T>, or calling cancel() explicitly, tears down the channel and then fires an out-of-band std::stop_token (ADR-017) — handed to whatever blocking work the producer side is doing (an HTTP read loop, a socket wait) so it can stop even while parked in I/O with nothing left to push.",
      cite: "include/agentengine/core/stream.hpp:147-193",
      href: gh("include/agentengine/core/stream.hpp"),
    },
    {
      id: "entry-make-stream",
      status: "real",
      tag: "make_stream<T>()",
      title: "One bounded channel, no actor addressing, no OPEN handshake",
      body:
        "make_stream<T>(mr, cfg) constructs a directly-connected producer/consumer pair over rt::make_channel<T, error>(cfg.capacity) — default capacity 256 — and a single std::stop_source shared by copy between both halves, so the consumer's cancel() is visible through the producer's stop_token() with no extra plumbing. chat_stream() is a plain synchronous call, not a coroutine: a conformer that streams for real hands the stream_producer<T> half to whatever background execution context performs the read loop (a detached std::thread today) and returns the stream<T> drain handle to the caller immediately.",
      cite: "include/agentengine/core/stream.hpp:219-228",
      href: gh("include/agentengine/core/stream.hpp"),
    },
    {
      id: "entry-response-update",
      status: "real",
      tag: "struct ChatResponseUpdate",
      title: "delta, is_final, and usage that only means something at the end",
      body:
        "delta is a ContentItem — the same type a whole ChatResponse's content is built from, so a drain loop accumulates updates into a normal message by appending each delta in order. is_final marks the terminal update for this call. usage is std::optional<Usage>, populated only when is_final is true and only when the backend actually reported usage for that call — nullopt means \"this backend/call provided none,\" and a caller that needs usage (AgentSession's streaming loop, 004 §5's TokenBudget<N>) must treat nullopt as a hard failure, never silently as zero cost. A third field, tool_call_argument_chunk, was added later (unified-streaming-design-draft.md §1 Piece B) for a companion update pushed as a tool call's own arguments grow; the drain loop skips appending delta when this is set, so a streamed tool call doesn't leave a spurious empty content item behind.",
      cite: "include/agentengine/core/chat_client.hpp:150-167",
      href: gh("include/agentengine/core/chat_client.hpp"),
    },
    {
      id: "entry-session-streaming",
      status: "real",
      tag: "AgentSession::set_stream_model_calls() / stream_model_calls()",
      title: "One flag that swaps the whole turn loop's model-call path",
      body:
        "Both are plain, noexcept accessors over a bool defaulting to false — every existing session is unaffected until it opts in (ADR-034). With it set, run_model_call() dispatches to the streaming call_stream() path instead of the plain chat() method, and each pushed ChatResponseUpdate is projected onto the session's own event stream as a run_event_kind::model_delta event — the mechanism the Events page covers end to end. A streamed run also emits one run_event_kind::warning right after run_started, because engaging stream_model_calls_ is itself an operator-visible choice worth surfacing, not a fabricated placeholder kind.",
      cite: "include/agentengine/rt/agent_session.hpp:719-720",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
  ],
  vi: [
    {
      id: "entry-chat-client-concept",
      status: "real",
      tag: "concept ChatClient",
      title: "Hai biểu thức bắt buộc, và chat_stream() là một trong số đó",
      body:
        "Kể từ ADR-035 Phase 3, concept này bắt buộc đúng capabilities() và chat_stream(request, ctx) -> stream<ChatResponseUpdate>; chat() trả về nguyên văn cả phản hồi đã cố ý không còn nằm trong hình dạng bắt buộc nữa. Điều này MỞ RỘNG những gì có thể tuân theo chứ không thu hẹp — mọi backend thật (OpenAIChatClient, AnthropicChatClient) vẫn có chat() và vẫn giữ nó, và việc nới lỏng này là thứ lần đầu tiên cho phép một backend giả định chỉ có chat_stream() biên dịch được với ChatClient. Một concept anh em có từ trước, LegacyChatClient, vẫn đòi cả hai phương thức — RecordingChatClient ràng buộc riêng theo nó vì thân hàm của chính nó gọi .chat() một cách vô điều kiện, và nếu ràng buộc theo ChatClient đã nới lỏng thì một bên tuân theo chỉ có chat_stream() sẽ biên dịch qua đó rồi hỏng sâu bên trong .chat() với một lỗi template khó hiểu thay vì một lỗi có tên.",
      cite: "include/agentengine/core/chat_client.hpp:205-209",
      href: gh("include/agentengine/core/chat_client.hpp"),
    },
    {
      id: "entry-stream-type",
      status: "real",
      tag: "class stream<T>",
      title: "Một handle rút cạn chỉ-di-chuyển trên một channel thật, không phải một coroutine",
      body:
        "stream<T> bọc rt::channel_consumer<T, error>. next() gọi thẳng try_pop() của channel — std::optional<T>, nullopt nghĩa là \"hiện chưa có gì trong bộ đệm, chưa xong,\" không bao giờ là tín hiệu để ngừng rút. done()/terminal()/fail_error() đọc trạng thái kết thúc của chính channel; gap_detected() được gán cứng false vì rt::channel<T,E> về mặt cấu trúc không thể làm rớt hay trùng lặp một mục. Buông một stream<T>, hoặc gọi cancel() rõ ràng, sẽ dỡ bỏ channel rồi phát ra một std::stop_token ngoài băng (ADR-017) — trao cho bất kỳ công việc chặn nào phía producer đang làm (một vòng đọc HTTP, một socket đang chờ) để nó có thể dừng lại ngay cả khi đang mắc kẹt trong I/O và không còn gì để đẩy.",
      cite: "include/agentengine/core/stream.hpp:147-193",
      href: gh("include/agentengine/core/stream.hpp"),
    },
    {
      id: "entry-make-stream",
      status: "real",
      tag: "make_stream<T>()",
      title: "Một channel có giới hạn, không định địa chỉ actor, không bắt tay OPEN",
      body:
        "make_stream<T>(mr, cfg) dựng một cặp producer/consumer nối trực tiếp trên rt::make_channel<T, error>(cfg.capacity) — dung lượng mặc định 256 — và một std::stop_source duy nhất được chia sẻ theo bản sao giữa cả hai phía, nên cancel() của consumer hiển thị qua stop_token() của producer mà không cần nối dây thêm. chat_stream() là một lệnh gọi đồng bộ thuần túy, không phải coroutine: một bên tuân theo streaming thật sự trao nửa stream_producer<T> cho bất kỳ ngữ cảnh thực thi nền nào thực hiện vòng đọc (một std::thread tách rời hôm nay) và trả ngay handle rút cạn stream<T> cho người gọi.",
      cite: "include/agentengine/core/stream.hpp:219-228",
      href: gh("include/agentengine/core/stream.hpp"),
    },
    {
      id: "entry-response-update",
      status: "real",
      tag: "struct ChatResponseUpdate",
      title: "delta, is_final, và usage chỉ có ý nghĩa ở lần cuối",
      body:
        "delta là một ContentItem — cùng kiểu mà nội dung của cả một ChatResponse được dựng từ đó, nên một vòng rút cạn tích lũy các update thành một message bình thường bằng cách nối từng delta theo thứ tự. is_final đánh dấu update cuối cùng của lệnh gọi này. usage là std::optional<Usage>, chỉ được điền khi is_final là true và chỉ khi backend thực sự báo cáo usage cho lệnh gọi đó — nullopt nghĩa là \"backend/lệnh gọi này không cung cấp gì cả,\" và một bên gọi cần usage (vòng lặp streaming của AgentSession, TokenBudget<N> của 004 §5) phải coi nullopt là một lỗi cứng, không bao giờ ngầm hiểu là chi phí bằng không. Một trường thứ ba, tool_call_argument_chunk, được thêm sau (unified-streaming-design-draft.md §1 Piece B) cho một update đồng hành được đẩy khi tham số của một lệnh gọi tool đang lớn dần; vòng rút cạn bỏ qua việc nối delta khi trường này được đặt, để một lệnh gọi tool dạng streaming không để lại một content item rỗng thừa.",
      cite: "include/agentengine/core/chat_client.hpp:150-167",
      href: gh("include/agentengine/core/chat_client.hpp"),
    },
    {
      id: "entry-session-streaming",
      status: "real",
      tag: "AgentSession::set_stream_model_calls() / stream_model_calls()",
      title: "Một cờ duy nhất đổi cả đường lệnh gọi model của vòng lặp lượt",
      body:
        "Cả hai đều là accessor noexcept đơn giản trên một bool mặc định false — mọi session sẵn có không bị ảnh hưởng cho tới khi tự chọn tham gia (ADR-034). Khi được bật, run_model_call() chuyển sang đường call_stream() dạng streaming thay vì phương thức chat() thuần túy, và mỗi ChatResponseUpdate được đẩy ra sẽ được chiếu lên luồng sự kiện riêng của session dưới dạng một sự kiện run_event_kind::model_delta — cơ chế mà trang Events trình bày trọn vẹn. Một lượt chạy dạng streaming cũng phát ra đúng một run_event_kind::warning ngay sau run_started, vì việc bật stream_model_calls_ tự nó là một lựa chọn đáng để người vận hành nhìn thấy, không phải một loại sự kiện bịa ra.",
      cite: "include/agentengine/rt/agent_session.hpp:719-720",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
  ],
};

// ------------------------------------------------------------------------------------------------
// Snippets (verbatim excerpts, trimmed for length — never invented)
// ------------------------------------------------------------------------------------------------

export const chatClientConceptSnippet = `// core/chat_client.hpp:205-209 -- the ADR-035 Phase 3 required shape
template <class T>
concept ChatClient = requires(T client, ChatRequest request, EffectContext& ctx) {
    { client.capabilities() } -> std::same_as<ChatClientCapabilities>;
    { client.chat_stream(request, ctx) } -> std::same_as<stream<ChatResponseUpdate>>;
};
// chat() is deliberately NOT required anymore -- every real backend still has one, this only
// WIDENS what can conform. LegacyChatClient (chat_client.hpp:211+) still requires both, and is
// what RecordingChatClient<Inner> gates Inner on instead, because its own body calls .chat().`;

export const chatResponseUpdateSnippet = `// core/chat_client.hpp:150-167
struct ChatResponseUpdate {
    ContentItem delta;
    bool        is_final = false;

    // ADR-034. Populated only when is_final == true, and only when the backend actually reported
    // usage for this call; nullopt means "this backend/call provided none" -- a caller that needs
    // usage (AgentSession's streaming loop, 004 §5's TokenBudget<N>) must treat nullopt as a hard
    // failure, never silently as zero cost.
    std::optional<Usage> usage = std::nullopt;

    // unified-streaming-design-draft.md §1 (Piece B). Set on a companion update pushed as a tool
    // call's arguments grow; delta is left at its default (empty ContentItem) on that update, and
    // the drain loop must skip appending delta when this is set.
    std::optional<ToolCallArgumentChunk> tool_call_argument_chunk = std::nullopt;
};`;

export const streamPrimitiveSnippet = `// core/stream.hpp:147-193 (trimmed) -- the consumer-side drain handle, 004 §1's literal
// ae::stream<T> return type of chat_stream()
template <class T>
class stream {
public:
    stream(stream const&)            = delete;   // move-only
    stream& operator=(stream const&) = delete;

    ~stream() { cancel(); }   // dropping the handle cancels the stream

    // std::nullopt when nothing is buffered right now -- drain to empty, then check done().
    [[nodiscard]] std::optional<T> next() { return inner_.try_pop(); }
    [[nodiscard]] bool             done() const noexcept { return inner_.done(); }

    [[nodiscard]] stream_terminal terminal() const noexcept { return inner_.terminal(); }
    [[nodiscard]] error           fail_error() const noexcept { return inner_.error().value_or(error{}); }
    [[nodiscard]] bool            gap_detected() const noexcept { return false; }  // structurally can't

    // ADR-017: tears the channel down, THEN fires an out-of-band std::stop_token -- handed to any
    // blocking work (an HTTP read loop, a socket wait) that would otherwise keep running after the
    // consumer is gone, without the producer having to attempt a push() first.
    void cancel() noexcept { inner_.cancel(); stop_.request_stop(); }

private:
    rt::channel_consumer<T, error> inner_;
    std::stop_source               stop_;
};

// core/stream.hpp:219-228 -- one bounded channel, no actor addressing, no OPEN handshake
template <class T>
[[nodiscard]] stream_pair<T> make_stream(std::pmr::memory_resource*, stream_config<T> cfg = {}) {
    auto pair = rt::make_channel<T, error>(cfg.capacity);   // default capacity: 256
    std::stop_source stop;   // ONE stop-state, shared by copy between both halves
    return stream_pair<T>{stream_producer<T>(std::move(pair.producer), stop),
                          stream<T>(std::move(pair.consumer), stop)};
}`;

export const streamingJokerConformerSnippet = `// examples/07_streaming.cpp:80-103 -- a real ChatClient conformer that streams for real: one
// word per push, from a background thread, through a real credit-controlled ring. Capacity 2 is
// deliberately smaller than the reply's word count, so draining genuinely exercises backpressure --
// a full ring stalls the producer thread, it never drops an item.
stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
    std::vector<std::string> const words =
        split_words("Why did the pirate take so long to learn the alphabet");

    stream_config<ChatResponseUpdate> cfg;
    cfg.capacity = 2;
    auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);

    if (producer_thread_.joinable()) producer_thread_.join();
    producer_thread_ = std::thread([producer = std::move(pair.producer), words]() mutable {
        for (std::size_t i = 0; i < words.size(); ++i) {
            ChatResponseUpdate update;
            ContentItem item;
            item.origin = content_origin::assistant;
            item.value  = Text{(i == 0 ? words[i] : " " + words[i])};
            update.delta    = std::move(item);
            update.is_final = (i + 1 == words.size());
            if (producer.push(std::move(update)) != stream_push::ok) return;  // consumer gone
        }
        producer.close();
    });

    return std::move(pair.consumer);
}`;

export const streamingDrainLoopSnippet = `// examples/07_streaming.cpp:112-143 -- the whole main(), the canonical drain loop
int main() {
    StreamingJokerChatClient client;
    EffectContext ctx;
    ctx.principal = Principal{"p-demo", ""};
    ChatRequest req;  // this fake client ignores the request and always streams the same reply

    stream<ChatResponseUpdate> s = client.chat_stream(req, ctx);

    std::string received;
    bool saw_final = false;
    while (!s.done()) {
        while (auto update = s.next()) {
            if (auto const* t = std::get_if<Text>(&update->delta.value)) {
                std::printf("%s", t->text.c_str());
                std::fflush(stdout);
                received += t->text;
            }
            if (update->is_final) saw_final = true;
        }
        if (!s.done()) std::this_thread::yield();  // ring momentarily empty, producer thread still live
    }
    std::printf("\\n");

    check(received == "Why did the pirate take so long to learn the alphabet",
          "the words arrived in order, exactly once each, across the thread boundary");
    check(saw_final, "the last update is marked is_final");
    check(s.terminal() == stream_terminal::closed, "the stream reached the success terminal");
    return g_failures == 0 ? 0 : 1;
}`;

export const sessionStreamingSnippet = `// examples/29_agent_session_events.cpp:117-133 (trimmed) -- opting a whole session into streaming
AgentSession<ScriptedChatClient> session;
session.initialize("s-events-demo", Principal{"p-demo", ""});
CapabilitySet const held = CapabilitySet::grant_root({});
session.set_capabilities(&held);

// The one flag that engages the streaming turn loop (ADR-034) -- without it, run_model_call()
// dispatches to the plain chat() method instead, and model_delta never fires.
session.set_stream_model_calls(true);

// Subscribed BEFORE start_run(): enable_event_stream() must be live when the run happens, or
// there is nothing to attach events to.
stream<RunEvent> events = session.enable_event_stream(std::pmr::get_default_resource());

auto result = drive(session.start_run(StartRun{user_message("hi")}));`;

export const openaiSseSnippet = `// What the socket delivers for an OpenAI-compatible stream -- "data:"-only lines, one compact
// JSON object each. openai::detail::split_sse_data_events, protocol/openai/chat_client.hpp:565-581
data: {"choices":[{"delta":{"content":"Ha"}}]}

data: {"choices":[{"delta":{"content":"noi"}}]}

data: {"choices":[],"usage":{"prompt_tokens":41,"completion_tokens":9}}

data: [DONE]

// -> two Text updates, 1:1 with the vendor's own chunks
// -> the trailing usage chunk has choices:[] and is captured BEFORE the empty-choices skip --
//    it exists only because build_request_body sent stream_options.include_usage; without it the
//    vendor's stream carries no usage at all, and ChatResponseUpdate::usage would stay nullopt.`;

export const anthropicSseSnippet = `// The same conversation, Anthropic's NAMED-event framing: an "event:" line PAIRED with the
// data: line after it. anthropic::detail::split_sse_named_events, protocol/anthropic/chat_client.hpp:722-743
event: message_start
data: {"message":{"usage":{"input_tokens":41,"output_tokens":1}}}

event: content_block_start
data: {"index":0,"content_block":{"type":"text"}}

event: content_block_delta
data: {"index":0,"delta":{"type":"text_delta","text":"Ha"}}

event: message_delta
data: {"usage":{"output_tokens":9}}

// -> a text_delta becomes a Text item ONLY if its own index was started as a "text" block
//    (ADR-035 hardening) -- a non-compliant Anthropic-wire gateway must not be able to inject a
//    stray Text through a mismatched block kind
// -> message_delta.usage is CUMULATIVE on this wire: output_tokens is OVERWRITTEN by each event,
//    never summed`;

export const workedExampleSnippet = `// examples/29_agent_session_events.cpp (trimmed) -- streaming through a whole session, not just
// a raw ChatClient. ScriptedChatClient pushes three word-deltas then closes; the session projects
// each one onto its own event stream as a model_delta, alongside the rest of the run's lifecycle.
session.set_stream_model_calls(true);
stream<RunEvent> events = session.enable_event_stream(std::pmr::get_default_resource());
auto result = drive(session.start_run(StartRun{user_message("hi")}));

std::string joined_deltas;
std::size_t delta_count = 0;
while (auto ev = events.next()) {
    if (ev->kind == run_event_kind::model_delta) {
        ++delta_count;
        auto const& d = std::get<run_event_payload::ModelDelta>(ev->payload);
        if (auto const* t = std::get_if<run_event_payload::ModelTextDelta>(&d.value)) {
            joined_deltas += t->text;
        }
    }
}

check(delta_count == 3, "exactly one model_delta event fired per pushed Text delta");
check(joined_deltas == "Hello, world!",
      "the model_delta events, joined in emission order, reconstruct the same text the final "
      "Message carries -- the event stream and the returned response never disagree");
check(text_of(result->message) == joined_deltas,
      "the final AgentResponse's message matches what the live event stream already showed");`;
