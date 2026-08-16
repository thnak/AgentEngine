// Content for the Durability detail page (api/durability.html). Same rule as apiContent.ts: every
// claim here is grounded in a real header, source file, RFC section, or test in THIS working tree,
// with a citation — don't invent a shape or a claim these sources don't make, and never blur "real
// and tested" with "a Reviewed RFC describes this but nothing implements it yet."
//
// This page is unusually exposed to that rule, because 019 is the RFC whose ground shifted most
// under ADR-037 (Quark's actor engine, its FenceToken, and its ReminderService were removed, not
// replaced). Where 019's own prose still claims a mechanism "carried over intact" and the code says
// otherwise, this file follows the CODE and says so out loud — see `durabilityGaps` below.
//
// Bilingual (EN/VI): every translatable field is a Record<Lang, T>. Code snippets, identifiers,
// RFC/ADR numbers, file paths, and error codes stay identical in both languages — only surrounding
// prose is translated.

import { gh } from "./apiContent";
import type { ApiEntry, ApiStatus } from "./apiContent";
import type { Lang } from "../i18n/LanguageContext";

// ---- right-rail TOC ---------------------------------------------------------------------------
// ids match the `id` attributes ApiDurabilityReference renders on each section.
export const durabilitySections: Record<Lang, { id: string; label: string }[]> = {
  en: [
    { id: "du-checkpoint", label: "What a checkpoint carries" },
    { id: "du-delete", label: "Delete, tombstone, receipt" },
    { id: "du-suspension", label: "Suspension & wake conditions" },
    { id: "du-effects", label: "Idempotency key & effect journal" },
    { id: "du-worked-flow", label: "One effect across a restart" },
    { id: "du-interactions", label: "Stopping for a human" },
    { id: "du-standing", label: "Standing effects & background work" },
    { id: "du-recovery", label: "Poison runs, node loss, deploys" },
    { id: "du-proofs", label: "The proofs" },
    { id: "du-gaps", label: "What is still missing" },
  ],
  vi: [
    { id: "du-checkpoint", label: "Checkpoint mang theo những gì" },
    { id: "du-delete", label: "Xóa, bia mộ, biên nhận" },
    { id: "du-suspension", label: "Treo & điều kiện đánh thức" },
    { id: "du-effects", label: "Khóa idempotency & nhật ký hiệu ứng" },
    { id: "du-worked-flow", label: "Một hiệu ứng qua một lần khởi động lại" },
    { id: "du-interactions", label: "Dừng lại vì một con người" },
    { id: "du-standing", label: "Hiệu ứng thường trực & việc chạy nền" },
    { id: "du-recovery", label: "Run độc, mất node, triển khai" },
    { id: "du-proofs", label: "Các bằng chứng" },
    { id: "du-gaps", label: "Những gì còn thiếu" },
  ],
};

// ---- one citation per section ------------------------------------------------------------------
export const durabilityEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "du-checkpoint",
      status: "real",
      tag: "rt::AgentSessionRecord · CheckpointCadence<N> · checkpoint_if_due()",
      title: "A checkpoint is a small, flat, JSON-encoded bookkeeping record — not the conversation",
      body:
        "rt::AgentSessionRecord carries seven fields and nothing else: session_id, principal_id, principal_tenant_id, a deleted flag, run_counter, turn_index, and the run's open Interactions. It is encoded as JSON through the session's own hand-written codec (never a reflection macro) and handed to a host-supplied rt::SessionStore as opaque bytes. History, typed session state, metadata, standing effects and pending CodeAct asks are all deliberately outside it — each for a named reason, not by omission. save_agent_session_snapshot() reads that record under the SAME rt::AsyncMutex every public entry point holds, so a checkpoint can never observe a half-mutated session; CheckpointCadence<N>::due() decides whether a given turn boundary writes at all, with the turn counter owned by the caller because AgentSession has no ambient store access (I2).",
      cite: "include/agentengine/rt/agent_session.hpp:389",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
    {
      id: "du-delete",
      status: "real",
      tag: "delete_session() → SessionDeletionReceipt",
      title: "Deletion writes a tombstone and hands back a receipt naming which half succeeded",
      body:
        "005 §6 asks for hard removal with a completion receipt. delete_session() writes an AgentSessionRecord{session_id, deleted=true} tombstone into the store — not a remove() — then clears in-process state through the locked path, filling in SessionDeletionReceipt{durable_record_removed, in_process_state_cleared} as each half lands. Either half can fail independently, which is exactly why the receipt names both. On the read side, load_agent_session_snapshot() maps a tombstoned record to std::nullopt, so a deleted session is indistinguishable from one that never existed. Restore is the mirror image and is narrow on purpose: restore_from_record() reinstates identity, run_counter, turn_index and open interactions, and re-derives last_run_id from the restored counter — so the next start_run() continues the original run sequence instead of re-minting a run id that already happened.",
      cite: "include/agentengine/rt/agent_session.hpp:1956",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
    {
      id: "du-suspension",
      status: "real",
      tag: "interaction_reason · BackgroundTaskDone · due_standing_effects()",
      title: "A suspended run is a returned coroutine plus a durable record — never a parked thread",
      body:
        "019 §2 wants a Suspended run to hold no activation, no sandbox, no connection, no thread. After ADR-037 there is no actor, no mailbox and no reminder service underneath a session at all, so this is closer to true by construction than it ever was: run_rounds() RETURNS when it suspends (with the kSuspendedForApproval sentinel), leaving only an Interaction record and whatever the host chooses to keep in memory. The cost of that shape is that nothing inside AgentEngine can wake a run by itself. Three of 019 §2's six wake rows have a real producer here — human input, local background-task completion, and timer/schedule — and each is delivered by a host calling start_run() or resolve_interaction() again. The other rows have no producer in this tree.",
      cite: "include/agentengine/rt/agent_session.hpp:1488",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
    {
      id: "du-effects",
      status: "real",
      tag: "derive_idempotency_key() · rt::EffectJournalEntry · authorize_reexecution()",
      title: "The key survives a restart because nothing in it comes from a clock or a random number",
      body:
        "019 §3's key is {run_id, turn_index, call_index, argument_digest}, and derive_idempotency_key() uses exactly those four inputs — argument_digest being an FNV-1a hash of the call's canonical JSON arguments. No wall clock, no randomness, no process identity: recompute it from a session rebuilt after a crash and you get the same string back. invoke_tool() derives it unconditionally on every call and stamps it into the ToolInvocationAudit record, so an effect is recognizable as the same effect afterwards. The journal is the other half: journal_effect_intent() appends before execution, journal_effect_outcome() after, both into an append-only rt::AppendLogStore log named session_id + \":effect_journal\", and unconfirmed_effect_intents() replays that log to answer which intents have no matching outcome. It answers only that — deciding what to DO about an unconfirmed intent (019 §7 G6's indeterminate) is deliberately not built.",
      cite: "include/agentengine/core/tool_pipeline.hpp:250",
      href: gh("include/agentengine/core/tool_pipeline.hpp"),
    },
    {
      id: "du-worked-flow",
      status: "real",
      tag: "test_effect_reexecution.cpp — F4-R7 … F4-R11",
      title: "make_payment, interrupted and replayed, with the real key at every step",
      body:
        "The rewind-and-replay path is proven end to end against a real invoke_tool() call in test_effect_reexecution.cpp: a MakePaymentTool declared EffectClass<at_most_once> runs once, and the SAME call is then re-run under the SAME idempotency key. Without an operator acknowledgement, authorize_reexecution() refuses BEFORE the pipeline is entered a second time, with the stable code effect.reexecution_requires_ack. With an acknowledgement, the re-execution runs and its audit record carries the identical key — recognizably a repeat of one effect, not a new, unrelated call. What the test does not do, and this page does not claim, is inject the fault itself: the interruption is manual, not a kill -9.",
      cite: "tests/test_effect_reexecution.cpp:109",
      href: gh("tests/test_effect_reexecution.cpp"),
    },
    {
      id: "du-interactions",
      status: "real",
      tag: "Interaction{interaction_id, run_id, reason} · rt/interaction_codec.hpp",
      title: "A run that waits for a human is a durability question, not just a UX one",
      body:
        "Interaction is the one internal correlation record behind every kind of waiting: input, auth, approval (ADR-029), and codeact_ask (ADR-057). It is all scalars and strings, so it round-trips through a checkpoint trivially — and it is the ONLY non-scalar field AgentSessionRecord carries, encoded by rt/interaction_codec.hpp. That makes an open approval durable and restorable. What is NOT durable is the round the approval was suspended in: resolve_interaction() requires history_'s tail to still be the exact assistant tool-call message that suspended, and history_ is not in the record. A restored session therefore knows an approval was pending and cannot resume it — ADR-029 named this residual itself. The approval flow's mechanics live on the AgentSession page; this is the persistence half of the same story.",
      cite: "include/agentengine/core/interaction.hpp:43",
      href: gh("include/agentengine/core/interaction.hpp"),
    },
    {
      id: "du-standing",
      status: "real",
      tag: "StandingEffect · start_background_task() · schedule_wakeup() · cancel_standing_effect()",
      title: "Authorized synchronously, executed detached, cancelled only by the principal that armed it",
      body:
        "006 §6b's one handle shape has two real producers here. background_task() runs the tool pipeline's resolve, authorize/bind, and approve steps SYNCHRONOUSLY on the calling thread — an undeclared-Backgroundable tool, a session-state-capturing tool, a missing capability, an exhausted Background<max_concurrent> grant, or a refused approval all fail before any thread is spawned. Only step 8 onward detaches; completion is pushed into a small mutex-guarded queue held behind a shared_ptr, which the next start_run()/resolve_interaction() drains under the session lock, attributing ToolCallFinished to the run that ASKED for the work. schedule_wakeup() arms a wake time computed from a caller-supplied now, gated three ways by the cap::Schedule grant. cancel_standing_effect() cancels bookkeeping only, and denies a caller whose principal id does not match the effect's owner. None of this survives a checkpoint: StandingEffect is in-memory-only by design and is not a field of AgentSessionRecord.",
      cite: "include/agentengine/core/standing_effect.hpp:18",
      href: gh("include/agentengine/core/standing_effect.hpp"),
    },
    {
      id: "du-recovery",
      status: "design",
      tag: "019 §4 — poison runs · node loss · deploys",
      title: "Restart on the same node is real; the rest of 019 §4 is not",
      body:
        "Process restart is the one recovery path with real machinery behind it: a store, a record, a restore, and a run sequence that never repeats itself. Poison runs are a policy, not a mechanism — the PoisonRunPolicy<N> predicate 019 §4 asks for has no home in any shipping header; it survives only as a dependency-free copy inside one test, which drives an always-failing chat client and confirms quarantine at exactly the bound with history intact. Quarantining is then the host's own bookkeeping decision, never a call into the session. Node loss is not handled at all, and 019 §4 says so in its own text: ADR-037 removed Quark's FenceToken and nothing replaced it, so there is no fence, epoch or lease anywhere in this tree, and five node-loss/fencing test files were retired as an accepted permanent gap. The deploy version-skew pin is unimplemented: AgentMetadata::agent_version exists, but nothing consults it when a run resumes.",
      cite: "tests/test_rt_agent_session_identity_and_admission.cpp:434",
      href: gh("tests/test_rt_agent_session_identity_and_admission.cpp"),
    },
    {
      id: "du-proofs",
      status: "real",
      tag: "tests/test_rt_*.cpp — deterministic, offline",
      title: "What each durability test actually asserts",
      body:
        "Every test below is a standalone int main() with a file-local check(cond, label): the label IS the claim, and each one runs deterministically with no live model and no network. Read the third column as the honest scope line — several of these are genuinely narrower than the 019 §7 gate they sit closest to, and are listed that way rather than rounded up.",
      cite: "tests/test_rt_agent_session_snapshot.cpp",
      href: gh("tests/test_rt_agent_session_snapshot.cpp"),
    },
    {
      id: "du-gaps",
      status: "design",
      tag: "019 §7 G1 … G6",
      title: "The gates 019 sets, and where this tree actually stands against them",
      body:
        "019's promotion gate is six items long. One of them (G1's restart-identical resume) is met for WORKFLOWS — killed at every one of 20 superstep boundaries, all 20 resumes byte-identical to the control — and not for sessions, because a session's record carries no history to resume from. The other five are either unbuilt, unmeasured, or contradicted by a mechanism ADR-037 removed. This is the honest scoreboard, including two places where 019's own prose is now out of date with the code.",
      cite: "019-Durability-and-Long-Running-Agents.md:102",
      href: gh("019-Durability-and-Long-Running-Agents.md"),
    },
  ],
  vi: [
    {
      id: "du-checkpoint",
      status: "real",
      tag: "rt::AgentSessionRecord · CheckpointCadence<N> · checkpoint_if_due()",
      title: "Một checkpoint là bản ghi sổ sách nhỏ, phẳng, mã hóa JSON — không phải cuộc hội thoại",
      body:
        "rt::AgentSessionRecord mang đúng bảy trường và không gì khác: session_id, principal_id, principal_tenant_id, cờ deleted, run_counter, turn_index, và các Interaction đang mở của run. Nó được mã hóa JSON bằng codec viết tay của chính session (không hề dùng macro phản chiếu) rồi trao cho một rt::SessionStore do host cung cấp dưới dạng byte mờ. Lịch sử hội thoại, state có kiểu, metadata, hiệu ứng thường trực và các câu hỏi CodeAct đang chờ đều cố ý nằm ngoài — mỗi thứ đều có lý do được nêu tên, không phải bị bỏ quên. save_agent_session_snapshot() đọc bản ghi đó dưới CÙNG một rt::AsyncMutex mà mọi điểm vào công khai đều giữ, nên một checkpoint không bao giờ nhìn thấy session đang bị sửa dở; CheckpointCadence<N>::due() quyết định một mốc lượt có ghi hay không, còn bộ đếm lượt thuộc về caller vì AgentSession không có quyền truy cập store ngầm (I2).",
      cite: "include/agentengine/rt/agent_session.hpp:389",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
    {
      id: "du-delete",
      status: "real",
      tag: "delete_session() → SessionDeletionReceipt",
      title: "Xóa thì ghi một bia mộ và trả lại biên nhận nói rõ nửa nào đã thành công",
      body:
        "005 §6 yêu cầu xóa cứng kèm một biên nhận hoàn tất. delete_session() ghi một bia mộ AgentSessionRecord{session_id, deleted=true} vào store — chứ không gọi remove() — rồi dọn trạng thái trong tiến trình qua đường có khóa, điền dần SessionDeletionReceipt{durable_record_removed, in_process_state_cleared} khi từng nửa hoàn tất. Mỗi nửa có thể hỏng độc lập, và đó chính là lý do biên nhận nêu tên cả hai. Ở phía đọc, load_agent_session_snapshot() ánh xạ một bản ghi có bia mộ thành std::nullopt, nên một session đã xóa không thể phân biệt với một session chưa từng tồn tại. Phục hồi là hình ảnh phản chiếu và hẹp một cách có chủ đích: restore_from_record() khôi phục danh tính, run_counter, turn_index và các interaction đang mở, rồi suy lại last_run_id từ bộ đếm vừa khôi phục — nhờ vậy start_run() kế tiếp đi tiếp dãy run gốc thay vì mint lại một run id đã từng xảy ra.",
      cite: "include/agentengine/rt/agent_session.hpp:1956",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
    {
      id: "du-suspension",
      status: "real",
      tag: "interaction_reason · BackgroundTaskDone · due_standing_effects()",
      title: "Một run đang treo là một coroutine đã trả về cộng bản ghi bền vững — không phải luồng bị ghim",
      body:
        "019 §2 muốn một run ở trạng thái Suspended không giữ activation, không sandbox, không kết nối, không luồng. Sau ADR-037, bên dưới một session không còn actor, không hòm thư, không dịch vụ nhắc giờ nào cả, nên điều này đúng theo cấu trúc hơn bao giờ hết: run_rounds() TRẢ VỀ khi treo (kèm mã sentinel kSuspendedForApproval), chỉ để lại một bản ghi Interaction và bất cứ thứ gì host chọn giữ trong bộ nhớ. Cái giá của hình dạng ấy là không thứ gì bên trong AgentEngine có thể tự đánh thức một run. Ba trong sáu dòng wake của 019 §2 có nguồn phát thật ở đây — con người nhập vào, tác vụ nền cục bộ hoàn tất, và bộ đếm giờ/lịch — và mỗi cái đều tới nơi nhờ host gọi lại start_run() hoặc resolve_interaction(). Những dòng còn lại không có nguồn phát nào trong cây mã này.",
      cite: "include/agentengine/rt/agent_session.hpp:1488",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
    {
      id: "du-effects",
      status: "real",
      tag: "derive_idempotency_key() · rt::EffectJournalEntry · authorize_reexecution()",
      title: "Khóa sống sót qua khởi động lại vì không thành phần nào của nó đến từ đồng hồ hay số ngẫu nhiên",
      body:
        "Khóa của 019 §3 là {run_id, turn_index, call_index, argument_digest}, và derive_idempotency_key() dùng đúng bốn đầu vào đó — argument_digest là hàm băm FNV-1a trên chuỗi JSON chuẩn tắc của tham số lời gọi. Không đồng hồ tường, không ngẫu nhiên, không danh tính tiến trình: tính lại từ một session dựng lại sau sự cố thì vẫn ra đúng chuỗi cũ. invoke_tool() suy ra khóa này vô điều kiện ở mọi lời gọi và đóng dấu vào bản ghi ToolInvocationAudit, nhờ vậy về sau vẫn nhận ra đó là cùng một hiệu ứng. Nhật ký là nửa còn lại: journal_effect_intent() ghi thêm TRƯỚC khi thực thi, journal_effect_outcome() ghi SAU, cả hai vào một log chỉ-ghi-thêm rt::AppendLogStore mang tên session_id + \":effect_journal\", và unconfirmed_effect_intents() phát lại log đó để trả lời ý định nào chưa có kết cục tương ứng. Nó chỉ trả lời đúng chừng ấy — quyết định phải LÀM GÌ với một ý định chưa xác nhận (điều bất định của 019 §7 G6) thì cố ý chưa xây.",
      cite: "include/agentengine/core/tool_pipeline.hpp:250",
      href: gh("include/agentengine/core/tool_pipeline.hpp"),
    },
    {
      id: "du-worked-flow",
      status: "real",
      tag: "test_effect_reexecution.cpp — F4-R7 … F4-R11",
      title: "make_payment, bị ngắt rồi phát lại, với khóa thật ở từng bước",
      body:
        "Đường tua-lại-rồi-chạy-lại được chứng minh đầu-cuối trên một lời gọi invoke_tool() thật trong test_effect_reexecution.cpp: một MakePaymentTool khai báo EffectClass<at_most_once> chạy một lần, rồi CHÍNH lời gọi đó được chạy lại dưới CHÍNH khóa idempotency cũ. Không có xác nhận của người vận hành, authorize_reexecution() từ chối TRƯỚC khi pipeline được vào lần thứ hai, với mã ổn định effect.reexecution_requires_ack. Có xác nhận rồi thì lần chạy lại diễn ra và bản ghi kiểm toán của nó mang đúng khóa cũ — nhận diện được là một lần lặp của một hiệu ứng, không phải một lời gọi mới không liên quan. Điều bài kiểm thử không làm, và trang này cũng không tuyên bố, là tự tiêm lỗi: sự gián đoạn ở đây là thủ công, không phải một kill -9.",
      cite: "tests/test_effect_reexecution.cpp:109",
      href: gh("tests/test_effect_reexecution.cpp"),
    },
    {
      id: "du-interactions",
      status: "real",
      tag: "Interaction{interaction_id, run_id, reason} · rt/interaction_codec.hpp",
      title: "Một run chờ con người là câu hỏi về tính bền vững, không chỉ về trải nghiệm",
      body:
        "Interaction là bản ghi tương quan nội bộ duy nhất đứng sau mọi kiểu chờ đợi: input, auth, approval (ADR-029) và codeact_ask (ADR-057). Nó toàn số và chuỗi, nên đi qua một checkpoint hết sức dễ dàng — và nó là trường KHÔNG vô hướng duy nhất mà AgentSessionRecord mang theo, mã hóa bởi rt/interaction_codec.hpp. Điều đó khiến một phê duyệt đang mở trở nên bền vững và khôi phục được. Thứ KHÔNG bền vững là chính vòng đã bị treo: resolve_interaction() đòi phần đuôi của history_ vẫn phải là đúng thông điệp gọi tool của assistant lúc treo, mà history_ lại không nằm trong bản ghi. Vì vậy một session vừa khôi phục biết rằng có một phê duyệt đang chờ nhưng không thể tiếp tục nó — chính ADR-029 đã nêu tên phần dư này. Cơ chế của luồng phê duyệt nằm ở trang AgentSession; đây là nửa bền vững của cùng câu chuyện.",
      cite: "include/agentengine/core/interaction.hpp:43",
      href: gh("include/agentengine/core/interaction.hpp"),
    },
    {
      id: "du-standing",
      status: "real",
      tag: "StandingEffect · start_background_task() · schedule_wakeup() · cancel_standing_effect()",
      title: "Cấp phép đồng bộ, thực thi tách rời, chỉ principal đã trang bị mới hủy được",
      body:
        "Một hình dạng handle duy nhất của 006 §6b có hai nguồn phát thật ở đây. background_task() chạy các bước phân giải, cấp phép/ràng buộc và phê duyệt của pipeline tool một cách ĐỒNG BỘ trên luồng gọi — một tool không khai báo Backgroundable, một tool bắt trạng thái session, thiếu capability, hết hạn mức Background<max_concurrent>, hay một phê duyệt bị từ chối, tất cả đều hỏng trước khi có luồng nào được sinh ra. Chỉ từ bước 8 trở đi mới tách rời; kết quả được đẩy vào một hàng đợi nhỏ có mutex nằm sau một shared_ptr, và lần start_run()/resolve_interaction() kế tiếp sẽ rút nó ra dưới khóa session, quy ToolCallFinished về đúng run đã YÊU CẦU công việc đó. schedule_wakeup() trang bị một thời điểm đánh thức tính từ tham số now do caller cung cấp, chặn ba đường theo grant cap::Schedule. cancel_standing_effect() chỉ hủy phần sổ sách, và từ chối caller có principal id không khớp chủ sở hữu. Không thứ nào ở trên sống sót qua một checkpoint: StandingEffect chỉ nằm trong bộ nhớ theo thiết kế và không phải một trường của AgentSessionRecord.",
      cite: "include/agentengine/core/standing_effect.hpp:18",
      href: gh("include/agentengine/core/standing_effect.hpp"),
    },
    {
      id: "du-recovery",
      status: "design",
      tag: "019 §4 — run độc · mất node · triển khai",
      title: "Khởi động lại trên cùng một node là có thật; phần còn lại của 019 §4 thì không",
      body:
        "Khởi động lại tiến trình là đường phục hồi duy nhất có bộ máy thật đứng sau: một store, một bản ghi, một lần khôi phục, và một dãy run không bao giờ lặp lại chính nó. Run độc là một chính sách chứ chưa phải cơ chế — vị từ PoisonRunPolicy<N> mà 019 §4 yêu cầu không có chỗ trong bất kỳ header nào được xuất bản; nó chỉ còn sống dưới dạng một bản sao không phụ thuộc bên trong một bài kiểm thử, bài này lái một chat client luôn hỏng và xác nhận việc cách ly xảy ra đúng tại ngưỡng với lịch sử còn nguyên. Việc cách ly sau đó là quyết định sổ sách của host, không bao giờ là một lời gọi vào session. Mất node hoàn toàn không được xử lý, và chính 019 §4 nói vậy trong văn bản của nó: ADR-037 đã bỏ FenceToken của Quark và không gì thay thế, nên trong cây mã này không có fence, epoch hay lease nào, và năm tệp kiểm thử về mất node/fencing đã bị rút lại như một khoảng trống vĩnh viễn được chấp nhận. Chính sách ghim phiên bản khi triển khai thì chưa hiện thực: AgentMetadata::agent_version có tồn tại, nhưng không gì tra tới nó khi một run tiếp tục.",
      cite: "tests/test_rt_agent_session_identity_and_admission.cpp:434",
      href: gh("tests/test_rt_agent_session_identity_and_admission.cpp"),
    },
    {
      id: "du-proofs",
      status: "real",
      tag: "tests/test_rt_*.cpp — tất định, ngoại tuyến",
      title: "Mỗi bài kiểm thử về tính bền vững thực sự khẳng định điều gì",
      body:
        "Mọi bài kiểm thử dưới đây là một int main() độc lập với một hàm check(cond, label) cục bộ trong tệp: nhãn CHÍNH LÀ tuyên bố, và mỗi bài chạy tất định, không model thật, không mạng. Hãy đọc cột thứ ba như dòng phạm vi trung thực — vài bài trong số này thực sự hẹp hơn cổng 019 §7 mà chúng gần nhất, và được liệt kê đúng như vậy thay vì được làm tròn lên.",
      cite: "tests/test_rt_agent_session_snapshot.cpp",
      href: gh("tests/test_rt_agent_session_snapshot.cpp"),
    },
    {
      id: "du-gaps",
      status: "design",
      tag: "019 §7 G1 … G6",
      title: "Những cổng 019 đặt ra, và cây mã này thực sự đứng ở đâu so với chúng",
      body:
        "Cổng thăng hạng của 019 dài sáu mục. Một mục (G1, tiếp tục giống hệt sau khởi động lại) đã đạt cho WORKFLOW — bị giết ở cả 20 mốc superstep, cả 20 lần tiếp tục đều giống hệt bản đối chứng từng byte — nhưng chưa đạt cho session, vì bản ghi của một session không mang lịch sử để mà tiếp tục. Năm mục còn lại thì hoặc chưa xây, hoặc chưa đo, hoặc bị mâu thuẫn bởi một cơ chế mà ADR-037 đã bỏ. Đây là bảng điểm trung thực, gồm cả hai chỗ mà văn bản của chính 019 giờ đã lệch so với mã.",
      cite: "019-Durability-and-Long-Running-Agents.md:102",
      href: gh("019-Durability-and-Long-Running-Agents.md"),
    },
  ],
};

// ---- checkpoint content table -----------------------------------------------------------------
export interface CheckpointField {
  field: string;
  carried: "yes" | "no";
  note: string;
  cite: string;
}

export const checkpointFields: Record<Lang, CheckpointField[]> = {
  en: [
    { field: "session_id · principal_id · principal_tenant_id", carried: "yes", note: "Identity, flattened to three strings — Principal is reconstructed from them on restore.", cite: "include/agentengine/rt/agent_session.hpp:874" },
    { field: "run_counter", carried: "yes", note: "The run's position. last_run_id is re-derived from it, so the next start_run() mints run:N+1 and never repeats a run id.", cite: "include/agentengine/rt/agent_session.hpp:878" },
    { field: "turn_index", carried: "yes", note: "Copied straight out of EffectContext — the same turn_index the idempotency key is derived from.", cite: "include/agentengine/rt/agent_session.hpp:869" },
    { field: "deleted", carried: "yes", note: "The tombstone flag. A record with it set reads back as std::nullopt, not as a session.", cite: "include/agentengine/rt/agent_session.hpp:1720" },
    { field: "open_interactions", carried: "yes", note: "The only non-scalar field, encoded by rt/interaction_codec.hpp. Pending approvals and input requests survive; the round they suspended does not.", cite: "include/agentengine/rt/agent_session.hpp:396" },
    { field: "history_ (the conversation)", carried: "no", note: "Named gap: no Message/ContentItem serialization in the record. ADR-043 adds a per-turn TurnDeltaRecord under require_durable, but nothing rehydrates history from it — load_turn_delta() has no caller.", cite: "include/agentengine/rt/agent_session.hpp:1818" },
    { field: "state_ · metadata_", carried: "no", note: "Same named gap as history — the typed StateT is arbitrary user data with no codec.", cite: "include/agentengine/rt/agent_session.hpp:383" },
    { field: "standing_effects_", carried: "no", note: "In-memory only by design. A schedule_wakeup armed just before a restart is lost — named in the header, not discovered later.", cite: "include/agentengine/core/standing_effect.hpp:18" },
    { field: "pending_codeact_asks_", carried: "no", note: "The stored script source/language/answers a suspended agent.ask() replays against. No codec, no field — the same scope ADR-029 left open.", cite: "include/agentengine/rt/agent_session.hpp:369" },
    { field: "capability handles", carried: "no", note: "019 §1 forbids it outright — a serialized capability handle would be a forgeable capability. capabilities_ is a bare host-owned pointer, so re-validation at resume is forced by construction.", cite: "019-Durability-and-Long-Running-Agents.md:15" },
    { field: "created_at_ns · updated_at_ns", carried: "no", note: "Deliberate narrowing: the session never had wall-clock members to round-trip, and this record refuses to invent them. No Clock capability is wired anywhere in the project.", cite: "include/agentengine/rt/agent_session.hpp:84" },
  ],
  vi: [
    { field: "session_id · principal_id · principal_tenant_id", carried: "yes", note: "Danh tính, dàn phẳng thành ba chuỗi — Principal được dựng lại từ chúng khi khôi phục.", cite: "include/agentengine/rt/agent_session.hpp:874" },
    { field: "run_counter", carried: "yes", note: "Vị trí của run. last_run_id được suy lại từ nó, nên start_run() kế tiếp mint run:N+1 và không bao giờ lặp một run id.", cite: "include/agentengine/rt/agent_session.hpp:878" },
    { field: "turn_index", carried: "yes", note: "Chép thẳng từ EffectContext — đúng turn_index mà khóa idempotency được suy ra từ đó.", cite: "include/agentengine/rt/agent_session.hpp:869" },
    { field: "deleted", carried: "yes", note: "Cờ bia mộ. Bản ghi bật cờ này đọc ra thành std::nullopt, chứ không phải một session.", cite: "include/agentengine/rt/agent_session.hpp:1720" },
    { field: "open_interactions", carried: "yes", note: "Trường không vô hướng duy nhất, mã hóa bởi rt/interaction_codec.hpp. Phê duyệt và yêu cầu nhập liệu đang chờ thì sống sót; vòng chạy đã treo thì không.", cite: "include/agentengine/rt/agent_session.hpp:396" },
    { field: "history_ (cuộc hội thoại)", carried: "no", note: "Khoảng trống được nêu tên: bản ghi không có phần tuần tự hóa Message/ContentItem. ADR-043 thêm TurnDeltaRecord theo từng lượt dưới require_durable, nhưng không gì nạp lại lịch sử từ đó — load_turn_delta() không có caller.", cite: "include/agentengine/rt/agent_session.hpp:1818" },
    { field: "state_ · metadata_", carried: "no", note: "Cùng khoảng trống với lịch sử — StateT có kiểu là dữ liệu tùy ý của người dùng, không có codec.", cite: "include/agentengine/rt/agent_session.hpp:383" },
    { field: "standing_effects_", carried: "no", note: "Chỉ nằm trong bộ nhớ theo thiết kế. Một schedule_wakeup vừa trang bị ngay trước khi khởi động lại sẽ mất — điều này được nêu ngay trong header, không phải phát hiện về sau.", cite: "include/agentengine/core/standing_effect.hpp:18" },
    { field: "pending_codeact_asks_", carried: "no", note: "Mã nguồn/ngôn ngữ/câu trả lời đã lưu để một agent.ask() đang treo phát lại. Không codec, không trường — cùng phạm vi mà ADR-029 để ngỏ.", cite: "include/agentengine/rt/agent_session.hpp:369" },
    { field: "capability handle", carried: "no", note: "019 §1 cấm thẳng — một capability handle được tuần tự hóa sẽ là một capability có thể giả mạo. capabilities_ chỉ là con trỏ trần do host sở hữu, nên việc thẩm định lại khi tiếp tục là bắt buộc theo cấu trúc.", cite: "019-Durability-and-Long-Running-Agents.md:15" },
    { field: "created_at_ns · updated_at_ns", carried: "no", note: "Thu hẹp có chủ đích: session vốn chưa từng có trường đồng hồ tường để mà lưu lại, và bản ghi này từ chối bịa ra chúng. Toàn dự án chưa nối capability Clock ở đâu cả.", cite: "include/agentengine/rt/agent_session.hpp:84" },
  ],
};

// ---- 019 §2 wake rows, scored against this tree -------------------------------------------------
export interface WakeRow {
  condition: string;
  mechanism: string;
  status: ApiStatus;
  cite: string;
}

export const wakeRows: Record<Lang, WakeRow[]> = {
  en: [
    { condition: "Human / caller input", mechanism: "resolve_interaction({interaction_id, approved, answer}) resumes the SAME run — proven for approval (SU3/SU4) and for agent.ask() replay (B3/B4).", status: "real", cite: "include/agentengine/rt/agent_session.hpp:635" },
    { condition: "Local background task completion", mechanism: "A detached worker pushes BackgroundTaskDone into a mutex-guarded queue; the next start_run()/resolve_interaction() drains it as its first statement, under the session lock.", status: "real", cite: "include/agentengine/rt/agent_session.hpp:575" },
    { condition: "Timer / schedule", mechanism: "schedule_wakeup(delay, label, now) arms a fire_at from a caller-supplied clock read; a HOST polls due_standing_effects(now). Nothing self-fires, and the armed effect is in-memory only — it does not survive a restart.", status: "real", cite: "include/agentengine/rt/agent_session.hpp:980" },
    { condition: "Manual resume", mechanism: "An operator calling start_run()/resolve_interaction() on a restored session — no separate mechanism, which is the point: the host owns residency.", status: "real", cite: "include/agentengine/rt/agent_session.hpp:573" },
    { condition: "External event", mechanism: "watch_resource is a standing_effect_kind enumerator with no producer anywhere in this tree. 019 §2 routes this row through 012 (A2A push / webhook / queue).", status: "design", cite: "include/agentengine/core/standing_effect.hpp:39" },
    { condition: "Remote task completion", mechanism: "No poller and no callback path exists for a remote A2A/MCP task to wake a suspended session. Named in the same header comment as the row above.", status: "design", cite: "include/agentengine/core/standing_effect.hpp:14" },
  ],
  vi: [
    { condition: "Con người / caller nhập vào", mechanism: "resolve_interaction({interaction_id, approved, answer}) tiếp tục CHÍNH run cũ — đã chứng minh cho phê duyệt (SU3/SU4) và cho phát lại agent.ask() (B3/B4).", status: "real", cite: "include/agentengine/rt/agent_session.hpp:635" },
    { condition: "Tác vụ nền cục bộ hoàn tất", mechanism: "Một worker tách rời đẩy BackgroundTaskDone vào hàng đợi có mutex; lần start_run()/resolve_interaction() kế tiếp rút nó ra ngay ở câu lệnh đầu tiên, dưới khóa session.", status: "real", cite: "include/agentengine/rt/agent_session.hpp:575" },
    { condition: "Bộ đếm giờ / lịch", mechanism: "schedule_wakeup(delay, label, now) đặt fire_at từ một lần đọc đồng hồ do caller cung cấp; HOST phải hỏi due_standing_effects(now). Không gì tự kích hoạt, và hiệu ứng đã trang bị chỉ nằm trong bộ nhớ — nó không sống sót qua khởi động lại.", status: "real", cite: "include/agentengine/rt/agent_session.hpp:980" },
    { condition: "Tiếp tục thủ công", mechanism: "Một người vận hành gọi start_run()/resolve_interaction() trên session vừa khôi phục — không có cơ chế riêng nào, và đó chính là điểm mấu chốt: host sở hữu việc cư trú trong bộ nhớ.", status: "real", cite: "include/agentengine/rt/agent_session.hpp:573" },
    { condition: "Sự kiện bên ngoài", mechanism: "watch_resource chỉ là một hằng liệt kê của standing_effect_kind, không có nguồn phát nào trong cây mã này. 019 §2 dẫn dòng này qua 012 (A2A push / webhook / hàng đợi).", status: "design", cite: "include/agentengine/core/standing_effect.hpp:39" },
    { condition: "Tác vụ từ xa hoàn tất", mechanism: "Không có bộ hỏi vòng lẫn đường callback nào để một tác vụ A2A/MCP từ xa đánh thức một session đang treo. Được nêu tên ngay trong cùng đoạn chú thích header với dòng trên.", status: "design", cite: "include/agentengine/core/standing_effect.hpp:14" },
  ],
};

// ---- effect classes -----------------------------------------------------------------------------
export interface EffectClassRow {
  cls: string;
  onReexecution: string;
  note: string;
}

export const effectClassRows: Record<Lang, EffectClassRow[]> = {
  en: [
    { cls: "pure", onReexecution: "Re-runs freely, no acknowledgement", note: "authorize_reexecution() returns success immediately. Also the only class that can auto-declassify a text_derived tool call at step 5." },
    { cls: "idempotent", onReexecution: "Re-runs freely, under its original key", note: "The key is what makes the repeat safe — and F1 already derived it, identically, before the restart." },
    { cls: "at_most_once", onReexecution: "Refused until an operator acknowledges", note: "effect.reexecution_requires_ack. operator_acknowledged is an explicit host/human bool — never inferred, never derived from model output (I3)." },
    { cls: "(undeclared)", onReexecution: "Treated as at_most_once", note: "The conservative direction, and the OPPOSITE default from declared_approval()'s least-restrictive one. A tool that forgot to classify itself is never silently safe to repeat." },
  ],
  vi: [
    { cls: "pure", onReexecution: "Chạy lại tự do, không cần xác nhận", note: "authorize_reexecution() trả về thành công ngay. Đây cũng là lớp duy nhất có thể tự giải mật một lời gọi tool text_derived ở bước 5." },
    { cls: "idempotent", onReexecution: "Chạy lại tự do, dưới khóa gốc của nó", note: "Chính khóa làm cho việc lặp lại an toàn — và F1 đã suy ra khóa đó, y hệt, từ trước khi khởi động lại." },
    { cls: "at_most_once", onReexecution: "Bị từ chối cho tới khi người vận hành xác nhận", note: "effect.reexecution_requires_ack. operator_acknowledged là một bool tường minh do host/con người cấp — không bao giờ suy đoán, không bao giờ dẫn xuất từ đầu ra của model (I3)." },
    { cls: "(không khai báo)", onReexecution: "Bị coi như at_most_once", note: "Hướng thận trọng, và là mặc định NGƯỢC lại với mặc định ít ràng buộc nhất của declared_approval(). Một tool quên tự phân loại thì không bao giờ mặc nhiên an toàn để lặp lại." },
  ],
};

// ---- the worked flow ----------------------------------------------------------------------------
export interface FlowStage {
  index: string;
  title: string;
  body: string;
  mono: string;
}

export const workedFlowStages: Record<Lang, FlowStage[]> = {
  en: [
    {
      index: "01",
      title: "The call is resolved and the key is derived — before anything executes",
      body: "invoke_tool() derives the idempotency key as its very first act, from the four inputs 019 §3 names and nothing else. For the make_payment call in test_effect_reexecution.cpp, run s-rewind:run:1 at turn 0, call index 0, arguments {\"account\":\"acct-1\"}, that is a fixed string — the FNV-1a digest below is what argument_digest() computes over the canonical JSON.",
      mono: "IdempotencyKey{\"s-rewind:run:1\", 0, 0, 7356067401319992899}\n → \"s-rewind:run:1:0:0:7356067401319992899\"",
    },
    {
      index: "02",
      title: "The intent is journaled before the effect happens",
      body: "journal_effect_intent() appends one flat entry — all scalars and strings, no variant, no serialization gap — into the append-only log named after the session. The append is immediately durable: there is no stage/commit two-phase dance, because an intent is a single terminal fact the instant it is written.",
      mono: "log \"s-rewind:effect_journal\" seq 1\n{\"idempotency_key\":\"s-rewind:run:1:0:0:7356067401319992899\",\n \"tool_name\":\"make_payment\",\"call_id\":\"call-1\",\n \"phase\":\"intent\",\"outcome_ok\":false}",
    },
    {
      index: "03",
      title: "The effect runs — and the process dies before the outcome is written",
      body: "This is 019 §7 G6's ambiguous moment: the payment may or may not have reached the outside world, and the journal cannot tell. What it CAN tell is that this exact key was attempted. On a file-backed log, everything appended before this point survives intact — only a torn trailing record can be lost, and read_from() stops cleanly before one rather than failing.",
      mono: "MakePaymentTool::invoke(...) → Reply{charged:true}\n  ✗ crash before journal_effect_outcome(...)",
    },
    {
      index: "04",
      title: "After the restart, the journal is replayed and the intent stands out",
      body: "unconfirmed_effect_intents() reads the whole log for the session and returns every intent with no matching outcome under the same key, in commit order. It is strictly a read: it answers WHICH intents need a decision, never what the decision should be. Deciding — 019 §7 G6's \"surface indeterminate, never guess\" — is deliberately not built here.",
      mono: "unconfirmed_effect_intents(store, \"s-rewind\")\n → [ {key …992899, make_payment, call-1, intent} ]",
    },
    {
      index: "05",
      title: "The re-execution is refused, and the refusal has a code",
      body: "Nothing about the crash makes the repeat safe. MakePaymentTool declares EffectClass<at_most_once>, so authorize_reexecution() refuses BEFORE the pipeline is entered a second time — the gate sits in front of invoke_tool(), not inside it. An undeclared tool would land in exactly the same branch, because undeclared defaults to at_most_once.",
      mono: "authorize_reexecution(at_most_once, /*ack=*/false)\n → error{policy, \"effect.reexecution_requires_ack\"}",
    },
    {
      index: "06",
      title: "A human acknowledges — and the repeat is still recognizably the same effect",
      body: "operator_acknowledged is an explicit bool supplied by the host or a human, never inferred and never derived from model output (I3). With it, the same call re-runs, and its audit record carries the identical key the first attempt carried: the system knows it repeated one effect rather than performing two.",
      mono: "authorize_reexecution(at_most_once, /*ack=*/true) → ok\naudit2.idempotency_key == audit1.idempotency_key",
    },
  ],
  vi: [
    {
      index: "01",
      title: "Lời gọi được phân giải và khóa được suy ra — trước khi bất cứ gì thực thi",
      body: "invoke_tool() suy ra khóa idempotency ngay ở hành động đầu tiên, từ đúng bốn đầu vào mà 019 §3 nêu tên và không gì khác. Với lời gọi make_payment trong test_effect_reexecution.cpp — run s-rewind:run:1, lượt 0, chỉ số lời gọi 0, tham số {\"account\":\"acct-1\"} — đó là một chuỗi cố định; phần băm FNV-1a bên dưới chính là thứ argument_digest() tính trên chuỗi JSON chuẩn tắc.",
      mono: "IdempotencyKey{\"s-rewind:run:1\", 0, 0, 7356067401319992899}\n → \"s-rewind:run:1:0:0:7356067401319992899\"",
    },
    {
      index: "02",
      title: "Ý định được ghi nhật ký trước khi hiệu ứng xảy ra",
      body: "journal_effect_intent() ghi thêm một bản ghi phẳng — toàn số và chuỗi, không variant, không khe hở tuần tự hóa — vào log chỉ-ghi-thêm mang tên session. Lần ghi thêm là bền vững ngay lập tức: không có màn hai pha stage/commit, vì một ý định là một sự kiện cuối cùng ngay khoảnh khắc nó được ghi.",
      mono: "log \"s-rewind:effect_journal\" seq 1\n{\"idempotency_key\":\"s-rewind:run:1:0:0:7356067401319992899\",\n \"tool_name\":\"make_payment\",\"call_id\":\"call-1\",\n \"phase\":\"intent\",\"outcome_ok\":false}",
    },
    {
      index: "03",
      title: "Hiệu ứng chạy — và tiến trình chết trước khi kết cục kịp ghi",
      body: "Đây đúng là khoảnh khắc bất định của 019 §7 G6: khoản thanh toán có thể đã hoặc chưa ra tới thế giới bên ngoài, và nhật ký không thể biết. Thứ nó BIẾT được là đúng khóa này đã từng được thử. Trên log lưu tệp, mọi thứ ghi trước điểm này đều nguyên vẹn — chỉ bản ghi cuối bị đứt dở mới có thể mất, và read_from() dừng gọn trước nó thay vì báo lỗi.",
      mono: "MakePaymentTool::invoke(...) → Reply{charged:true}\n  ✗ sập trước journal_effect_outcome(...)",
    },
    {
      index: "04",
      title: "Sau khi khởi động lại, nhật ký được phát lại và ý định lộ ra",
      body: "unconfirmed_effect_intents() đọc toàn bộ log của session và trả về mọi ý định chưa có kết cục tương ứng dưới cùng một khóa, theo thứ tự ghi. Nó thuần là một phép đọc: nó trả lời NHỮNG ý định nào cần một quyết định, chứ không bao giờ nói quyết định phải là gì. Việc quyết định — \"nêu ra là bất định, không bao giờ đoán\" của 019 §7 G6 — cố ý chưa được xây ở đây.",
      mono: "unconfirmed_effect_intents(store, \"s-rewind\")\n → [ {key …992899, make_payment, call-1, intent} ]",
    },
    {
      index: "05",
      title: "Lần chạy lại bị từ chối, và lời từ chối có mã riêng",
      body: "Không có gì trong cú sập khiến việc lặp lại trở nên an toàn. MakePaymentTool khai báo EffectClass<at_most_once>, nên authorize_reexecution() từ chối TRƯỚC khi pipeline được vào lần thứ hai — cổng chặn nằm phía trước invoke_tool(), không nằm bên trong. Một tool không khai báo cũng rơi đúng vào nhánh này, vì không khai báo thì mặc định là at_most_once.",
      mono: "authorize_reexecution(at_most_once, /*ack=*/false)\n → error{policy, \"effect.reexecution_requires_ack\"}",
    },
    {
      index: "06",
      title: "Một con người xác nhận — và lần lặp vẫn nhận diện được là cùng một hiệu ứng",
      body: "operator_acknowledged là một bool tường minh do host hoặc con người cấp, không bao giờ suy đoán và không bao giờ dẫn xuất từ đầu ra của model (I3). Có nó rồi, đúng lời gọi ấy chạy lại, và bản ghi kiểm toán mang đúng khóa mà lần đầu đã mang: hệ thống biết mình đã lặp lại một hiệu ứng chứ không phải thực hiện hai.",
      mono: "authorize_reexecution(at_most_once, /*ack=*/true) → ok\naudit2.idempotency_key == audit1.idempotency_key",
    },
  ],
};

// ---- the proofs ---------------------------------------------------------------------------------
export interface ProofRow {
  test: string;
  asserts: string;
  scope: string;
}

export const proofRows: Record<Lang, ProofRow[]> = {
  en: [
    {
      test: "test_rt_agent_session_checkpoint_restart.cpp — D1-R1…R4",
      asserts: "A session checkpointed after turn 2 restores into a FRESH instance whose next start_run() mints s-ckpt:run:3 — never re-minting run:1 or run:2.",
      scope: "This is what \"restart-identical resume\" amounts to for a session today: the run SEQUENCE is preserved, not the conversation. No kill -9, no history.",
    },
    {
      test: "test_rt_agent_session_snapshot.cpp — P1…P6",
      asserts: "Record round-trips; a second save under the same id wins; delete_session() returns a both-halves-done receipt and the deleted session then reads back exactly like one that never existed; checkpoint_if_due() below the cadence writes NOTHING; a concurrent snapshot_record() queues behind an in-flight start_run() and observes post-run state.",
      scope: "P6 is the replacement for Quark's FenceToken: a real serialization proof, in-process only.",
    },
    {
      test: "test_rt_agent_session_checkpoint_cadence.cpp — D2-R1…R3",
      asserts: "CheckpointCadence<3> writes at exactly turns 3 and 6 out of 7 (5 writes skipped), and the durable record then honestly reads run_counter == 6 while the live session is at run:7.",
      scope: "019 §1's \"cost is bounded\" as a policy, with the live/durable gap observable rather than hidden.",
    },
    {
      test: "test_rt_session_store.cpp — M1…M5, F1…F7",
      asserts: "save/load/exists/remove contract, not-found as a real error code, idempotent remove, overwrite-not-append; and for FileSessionStore, a brand-new instance over the same root reads what a destroyed instance wrote, plus rejection of ids containing a separator or \"..\".",
      scope: "Durability across a process boundary is proven for the file store. Torn-write safety is NOT — save() truncates in place, named in the header.",
    },
    {
      test: "test_rt_append_log_store.cpp — L1…L7",
      asserts: "Strictly increasing seq numbers, exclusive read_from(id, N), per-log isolation, cross-instance file durability, and a partially-written trailing record that read_from() stops cleanly before instead of failing.",
      scope: "The journal's storage substrate, proven standalone.",
    },
    {
      test: "test_rt_effect_journal.cpp — J1…J8",
      asserts: "Intent then outcome land in commit order carrying the real key; an intent with no outcome is reported unconfirmed and a confirmed one never is; with two effects only the interrupted one is reported, distinguished by key; two sessions' journals in one store never collide.",
      scope: "The reconciliation PRIMITIVE. No fault injector, and no decision layer on top of it.",
    },
    {
      test: "test_idempotency_key.cpp — F1-C1, F1-R1…R6",
      asserts: "A key recomputed from a completely fresh EffectContext and freshly parsed arguments (a simulated restart) is identical; changing any ONE of the four inputs changes the key; and invoke_tool()'s own audit key equals what derive_idempotency_key() computes.",
      scope: "F1-R6 is what makes this more than an unused helper — the real pipeline is wired to it.",
    },
    {
      test: "test_effect_reexecution.cpp — F4-C1…F4-R11",
      asserts: "An undeclared tool defaults to at_most_once; pure/idempotent re-run freely; at_most_once is blocked with effect.reexecution_requires_ack and allowed with an ack; and a real second invoke_tool() call carries the identical key as the first.",
      scope: "019 §6's rewind rule, proven against a manually triggered replay rather than a workflow rewind.",
    },
    {
      test: "test_rt_agent_session_suspend_approval.cpp — SU1…SU6",
      asserts: "The gated tool's invoke() is never reached; an approval Interaction opens naming the suspending run; a second start_run() is refused with run.approval_pending and mints no run id; approve resumes the SAME run and denial folds an ordinary tool error into history; a resolve from a foreign principal is denied and leaves the interaction OPEN.",
      scope: "All in-process. Nothing here crosses a restart.",
    },
    {
      test: "test_agent_session_suspend_codeact_ask.cpp — B1…B7",
      asserts: "agent.ask() suspends with its own reason tag and prompt; the same interaction_id is reused across chained questions; resolving replays the stored script; and B7 proves the cost of that design — a mediated file write before the ask runs TWICE in total (\"XX\", not \"X\").",
      scope: "B7 is a residual proven, not asserted away: replay re-executes the deterministic prefix, side effects included.",
    },
    {
      test: "test_rt_agent_session_identity_and_admission.cpp — POISON-1…4",
      asserts: "An always-failing chat client is quarantined at EXACTLY the bound (3 attempts), never earlier or later; history keeps one entry per failed attempt; session identity is untouched.",
      scope: "Against a test-local copy of PoisonRunPolicy<N> — the shipping headers contain no such policy at all.",
    },
    {
      test: "test_rt_workflow_checkpoint_g2.cpp — G2",
      asserts: "A 20-node workflow is killed at EVERY one of its 20 superstep boundaries; every single resume, from a genuinely new supervisor, completes with output byte-identical to the uninterrupted control.",
      scope: "The strongest restart-identity proof in the repo — and it is 014's, for workflows, not 019's for sessions.",
    },
    {
      test: "test_rt_project_manifest.cpp — P1…P5 · test_rt_project_scale_isolation.cpp — G1(a)",
      asserts: "pause_project() checkpoints every member first and flips status to paused ONLY if all checkpoints succeeded — a failing member store leaves the project active rather than falsely paused; and pausing one Project out of 100 writes to none of the other 99 stores.",
      scope: "ADR-038's \"pause is checkpoint, full stop\": there is no eviction and no reactivation to measure.",
    },
    {
      test: "test_rt_agent_session_ack_policy.cpp — T1…T3",
      asserts: "ack_policy::at_most_once never touches the store; require_durable writes the turn delta AND the session record before the caller sees a response; a failed durable write surfaces as run.durable_ack_failed rather than a false success.",
      scope: "005 §2's ack contract for one turn's delta — not whole-conversation durability, and nothing reads the delta back.",
    },
    {
      test: "test_rt_agent_session_background_task.cpp · test_rt_agent_session_schedule_wakeup.cpp",
      asserts: "A non-Backgroundable tool registers nothing; the calling turn returns well before the tool's own sleep; a second task past Background<1> is refused; a foreign principal cannot cancel; completions are drained by the next start_run() with no explicit call. For timers: no grant, over-horizon and over-capacity each fail closed, fire_at is computed from the caller's now, and due_standing_effects() is a pure read.",
      scope: "Both surfaces are real and enforced — and neither survives a checkpoint.",
    },
  ],
  vi: [
    {
      test: "test_rt_agent_session_checkpoint_restart.cpp — D1-R1…R4",
      asserts: "Một session được checkpoint sau lượt 2 khôi phục vào một thể hiện MỚI TINH, và start_run() kế tiếp mint s-ckpt:run:3 — không bao giờ mint lại run:1 hay run:2.",
      scope: "Đó là tất cả những gì \"tiếp tục giống hệt sau khởi động lại\" có nghĩa với một session hôm nay: DÃY run được giữ, còn cuộc hội thoại thì không. Không kill -9, không lịch sử.",
    },
    {
      test: "test_rt_agent_session_snapshot.cpp — P1…P6",
      asserts: "Bản ghi đi về nguyên vẹn; lần save thứ hai cùng id thì thắng; delete_session() trả biên nhận đủ hai nửa và session đã xóa đọc ra y hệt một session chưa từng tồn tại; checkpoint_if_due() dưới ngưỡng nhịp KHÔNG ghi gì; một snapshot_record() song song xếp hàng sau start_run() đang chạy và nhìn thấy trạng thái sau khi run xong.",
      scope: "P6 là thứ thay thế FenceToken của Quark: một bằng chứng tuần tự hóa thật, nhưng chỉ trong một tiến trình.",
    },
    {
      test: "test_rt_agent_session_checkpoint_cadence.cpp — D2-R1…R3",
      asserts: "CheckpointCadence<3> ghi đúng ở lượt 3 và 6 trong 7 lượt (bỏ qua 5 lần ghi), và bản ghi bền vững sau đó thành thật hiển thị run_counter == 6 trong khi session sống đang ở run:7.",
      scope: "\"Chi phí có chặn\" của 019 §1 như một chính sách, với khoảng lệch sống/bền vững quan sát được chứ không bị giấu.",
    },
    {
      test: "test_rt_session_store.cpp — M1…M5, F1…F7",
      asserts: "Hợp đồng save/load/exists/remove, not-found là một mã lỗi thật, remove lũy đẳng, ghi đè chứ không nối thêm; và với FileSessionStore, một thể hiện mới tinh trên cùng thư mục gốc đọc được thứ mà thể hiện đã hủy từng ghi, kèm việc từ chối id chứa dấu phân cách hay \"..\".",
      scope: "Tính bền vững qua ranh giới tiến trình đã được chứng minh cho store tệp. An toàn trước ghi đứt dở thì CHƯA — save() cắt cụt tại chỗ, điều này được nêu ngay trong header.",
    },
    {
      test: "test_rt_append_log_store.cpp — L1…L7",
      asserts: "Số seq tăng nghiêm ngặt, read_from(id, N) loại trừ N, các log cô lập nhau, bền vững qua thể hiện tệp mới, và một bản ghi cuối ghi dở thì read_from() dừng gọn trước nó thay vì báo lỗi.",
      scope: "Nền lưu trữ của nhật ký, được chứng minh độc lập.",
    },
    {
      test: "test_rt_effect_journal.cpp — J1…J8",
      asserts: "Ý định rồi kết cục nằm đúng thứ tự ghi và mang khóa thật; một ý định không kết cục bị báo là chưa xác nhận còn cái đã xác nhận thì không; với hai hiệu ứng, chỉ cái bị ngắt được báo, phân biệt bằng khóa; nhật ký của hai session trong một store không bao giờ lẫn nhau.",
      scope: "Đây là NGUYÊN LIỆU đối chiếu. Không có bộ tiêm lỗi, và không có tầng quyết định đặt lên trên.",
    },
    {
      test: "test_idempotency_key.cpp — F1-C1, F1-R1…R6",
      asserts: "Khóa tính lại từ một EffectContext hoàn toàn mới và tham số vừa phân tích lại (mô phỏng khởi động lại) thì giống hệt; đổi MỘT trong bốn đầu vào là khóa đổi theo; và khóa trong bản ghi kiểm toán của chính invoke_tool() bằng đúng thứ derive_idempotency_key() tính ra.",
      scope: "F1-R6 là thứ khiến đây không chỉ là một hàm tiện ích bỏ không — pipeline thật đã nối vào nó.",
    },
    {
      test: "test_effect_reexecution.cpp — F4-C1…F4-R11",
      asserts: "Tool không khai báo mặc định thành at_most_once; pure/idempotent chạy lại tự do; at_most_once bị chặn với effect.reexecution_requires_ack và được cho qua khi có xác nhận; và lời gọi invoke_tool() thứ hai thật sự mang đúng khóa của lần đầu.",
      scope: "Luật tua lại của 019 §6, chứng minh trên một lần phát lại kích hoạt thủ công chứ không phải một lần tua workflow.",
    },
    {
      test: "test_rt_agent_session_suspend_approval.cpp — SU1…SU6",
      asserts: "invoke() của tool bị chặn không bao giờ được chạm tới; một Interaction phê duyệt mở ra và nêu tên đúng run đang treo; start_run() thứ hai bị từ chối với run.approval_pending và không mint run id nào; chấp thuận thì tiếp tục CHÍNH run cũ còn từ chối thì gấp một lỗi tool thường vào lịch sử; một lệnh resolve từ principal lạ bị từ chối và để interaction VẪN MỞ.",
      scope: "Tất cả đều trong một tiến trình. Không điều gì ở đây đi qua một lần khởi động lại.",
    },
    {
      test: "test_agent_session_suspend_codeact_ask.cpp — B1…B7",
      asserts: "agent.ask() treo lại với thẻ lý do và câu hỏi riêng; cùng một interaction_id được tái dùng qua các câu hỏi nối tiếp; giải quyết thì phát lại đoạn script đã lưu; và B7 chứng minh cái giá của thiết kế ấy — một lần ghi tệp có trung gian đứng trước câu hỏi chạy TỔNG CỘNG HAI LẦN (\"XX\", không phải \"X\").",
      scope: "B7 là một phần dư được chứng minh chứ không bị nói cho qua: phát lại chạy lại toàn bộ đoạn tất định phía trước, kể cả tác dụng phụ.",
    },
    {
      test: "test_rt_agent_session_identity_and_admission.cpp — POISON-1…4",
      asserts: "Một chat client luôn hỏng bị cách ly đúng TẠI ngưỡng (3 lần thử), không sớm cũng không muộn; lịch sử giữ đúng một mục cho mỗi lần thử hỏng; danh tính session không bị đụng tới.",
      scope: "Chạy trên một bản sao PoisonRunPolicy<N> cục bộ trong tệp kiểm thử — các header được xuất bản không hề chứa chính sách này.",
    },
    {
      test: "test_rt_workflow_checkpoint_g2.cpp — G2",
      asserts: "Một workflow 20 nút bị giết ở CẢ 20 mốc superstep; mọi lần tiếp tục, từ một supervisor mới hoàn toàn, đều cho ra kết quả giống hệt bản đối chứng không bị ngắt tới từng byte.",
      scope: "Bằng chứng tiếp-tục-giống-hệt mạnh nhất trong kho mã — và nó thuộc về 014, cho workflow, chứ không phải 019 cho session.",
    },
    {
      test: "test_rt_project_manifest.cpp — P1…P5 · test_rt_project_scale_isolation.cpp — G1(a)",
      asserts: "pause_project() checkpoint mọi thành viên trước rồi CHỈ lật trạng thái sang paused nếu tất cả checkpoint thành công — một store thành viên bị hỏng sẽ khiến project vẫn ở active thay vì bị báo paused sai sự thật; và tạm dừng một Project trong 100 cái không ghi vào bất kỳ store nào của 99 cái còn lại.",
      scope: "Đúng tinh thần \"pause là checkpoint, hết\" của ADR-038: không có việc trục xuất khỏi bộ nhớ lẫn kích hoạt lại để mà đo.",
    },
    {
      test: "test_rt_agent_session_ack_policy.cpp — T1…T3",
      asserts: "ack_policy::at_most_once không hề chạm vào store; require_durable ghi delta của lượt VÀ bản ghi session trước khi caller thấy phản hồi; một lần ghi bền vững hỏng thì nổi lên thành run.durable_ack_failed chứ không phải một thành công giả.",
      scope: "Hợp đồng ack của 005 §2 cho delta một lượt — không phải tính bền vững cho cả cuộc hội thoại, và cũng chưa có ai đọc delta đó về.",
    },
    {
      test: "test_rt_agent_session_background_task.cpp · test_rt_agent_session_schedule_wakeup.cpp",
      asserts: "Một tool không Backgroundable thì không đăng ký gì; lượt gọi trả về sớm hơn hẳn thời gian ngủ của tool; tác vụ thứ hai vượt Background<1> bị từ chối; principal lạ không hủy được; kết quả được rút ra bởi start_run() kế tiếp mà không cần gọi gì thêm. Với bộ đếm giờ: không có grant, quá chân trời và quá sức chứa đều từ chối đóng, fire_at tính từ now của caller, và due_standing_effects() là một phép đọc thuần.",
      scope: "Cả hai bề mặt đều có thật và được cưỡng chế — và không cái nào sống sót qua một checkpoint.",
    },
  ],
};

// ---- the honest gap list -------------------------------------------------------------------------
export interface GapRow {
  item: string;
  state: string;
  cite: string;
}

export const durabilityGaps: Record<Lang, GapRow[]> = {
  en: [
    { item: "G1 — kill -9 at every checkpoint boundary, resume output identical to the control", state: "Met for workflows (20/20 boundaries, byte-identical), NOT for sessions: the session record carries no history, and ADR-037 retired test_session_restart_identical_resume.cpp as an accepted gap rather than porting it.", cite: "decisions/ADR-037-remove-quark-as-core-runtime.md:237" },
    { item: "G2 — an interrupted idempotent effect retried exactly once over 10⁴ fault-injected trials", state: "No fault injector exists anywhere in tests/. The journal proves the primitive; nothing drives it under injected duplication or delay.", cite: "tests/test_rt_effect_journal.cpp:1" },
    { item: "G3 — a suspended run's resident cost measured by census: no activation, no sandbox, no connection, no thread", state: "No census test exists; the M4-era test_suspended_zero_resources_e2e.cpp was retired with Quark. Structurally an rt::AgentSession owns no thread or timer at all — but that is an argument from the code, not the measurement 019 asks for.", cite: "decisions/ADR-037-remove-quark-as-core-runtime.md:237" },
    { item: "G4 — 10⁶ reminders due simultaneously wake without a thundering herd", state: "Unreachable: there is no reminder mechanism. 019 §2 and G4 both still say the durable reminders \"carried over intact\" into rt::, and the code disagrees — ADR-037 deleted Quark's ReminderService and rt:: has never had any timer primitive; schedule_wakeup is host-polled.", cite: "include/agentengine/rt/agent_session.hpp:148" },
    { item: "G5 — a poison run quarantined after its bound, state intact, operator-visible reason", state: "Half met. The bound-and-preserve behaviour is proven, but PoisonRunPolicy<N> exists only as a copy inside a test file — no shipping header defines it, and nothing surfaces an operator-visible reason.", cite: "tests/test_rt_agent_session_identity_and_admission.cpp:108" },
    { item: "G6 — an at-most-once effect interrupted at the ambiguous instant surfaces indeterminate, 10⁴ trials", state: "Deliberately not built. unconfirmed_effect_intents() answers which intents lack an outcome — a read, never a decision. The decision layer is named as separately-scoped work.", cite: "include/agentengine/rt/effect_journal.hpp:30" },
    { item: "The effect journal is not wired into the turn loop", state: "journal_effect_intent()/journal_effect_outcome()/authorize_reexecution() have NO caller outside their own tests. run_rounds() derives and audits the idempotency key on every call, but journals nothing. A host wanting 019 §3's discipline must call the journal itself.", cite: "include/agentengine/rt/agent_session.hpp:1512" },
    { item: "Node loss, fencing, leases, epochs", state: "None exist, by decision. 019 §4 now names this a real, permanent gap; there is no multi-node story and no compare-and-set primitive in either store.", cite: "019-Durability-and-Long-Running-Agents.md:73" },
    { item: "FileSessionStore has no atomic write", state: "save() truncates and rewrites in place — a crash mid-write can leave torn bytes. The append log does better by construction (only a torn tail is lost, and reads stop cleanly before it). Named in the header, not discovered in production.", cite: "include/agentengine/rt/session_store.hpp:149" },
    { item: "019 §4 deploy version-skew pin", state: "Not implemented. AgentMetadata::agent_version is real (ADR-044), but nothing consults it when a run resumes, so there is no pin-vs-migrate decision to make.", cite: "include/agentengine/core/agent_registry.hpp:63" },
    { item: "019 §5 retention and garbage collection", state: "No retention policy, no GC, no grace period anywhere — the project manifest header says so in as many words for the archive path.", cite: "include/agentengine/rt/project_manifest.hpp:242" },
  ],
  vi: [
    { item: "G1 — kill -9 tại mọi mốc checkpoint, kết quả tiếp tục giống hệt bản đối chứng", state: "Đạt cho workflow (20/20 mốc, giống hệt từng byte), CHƯA đạt cho session: bản ghi session không mang lịch sử, và ADR-037 đã rút test_session_restart_identical_resume.cpp như một khoảng trống được chấp nhận thay vì chuyển đổi nó.", cite: "decisions/ADR-037-remove-quark-as-core-runtime.md:237" },
    { item: "G2 — hiệu ứng idempotent bị ngắt được thử lại đúng một lần qua 10⁴ lần tiêm lỗi", state: "Không có bộ tiêm lỗi nào trong tests/. Nhật ký chứng minh nguyên liệu; không gì lái nó dưới cảnh nhân bản hay trì hoãn được tiêm vào.", cite: "tests/test_rt_effect_journal.cpp:1" },
    { item: "G3 — chi phí cư trú của một run đang treo đo bằng kiểm đếm: không activation, không sandbox, không kết nối, không luồng", state: "Không có bài kiểm đếm nào; test_suspended_zero_resources_e2e.cpp thời M4 đã bị rút cùng Quark. Về cấu trúc, một rt::AgentSession không sở hữu luồng hay bộ đếm giờ nào — nhưng đó là lập luận từ mã, không phải phép đo mà 019 yêu cầu.", cite: "decisions/ADR-037-remove-quark-as-core-runtime.md:237" },
    { item: "G4 — 10⁶ nhắc giờ cùng đến hạn mà không gây bão đánh thức", state: "Không thể chạm tới: không có cơ chế nhắc giờ nào. Cả 019 §2 lẫn G4 vẫn nói rằng phần nhắc giờ bền vững đã \"được chuyển nguyên vẹn\" sang rt::, còn mã thì nói ngược lại — ADR-037 đã xóa ReminderService của Quark và rt:: chưa từng có nguyên liệu đếm giờ nào; schedule_wakeup do host hỏi vòng.", cite: "include/agentengine/rt/agent_session.hpp:148" },
    { item: "G5 — một run độc bị cách ly sau ngưỡng, trạng thái còn nguyên, lý do người vận hành thấy được", state: "Đạt một nửa. Hành vi dừng-tại-ngưỡng và giữ nguyên trạng thái đã được chứng minh, nhưng PoisonRunPolicy<N> chỉ tồn tại như một bản sao trong một tệp kiểm thử — không header xuất bản nào định nghĩa nó, và không gì nêu ra lý do cho người vận hành thấy.", cite: "tests/test_rt_agent_session_identity_and_admission.cpp:108" },
    { item: "G6 — hiệu ứng at-most-once bị ngắt đúng khoảnh khắc bất định thì phải nêu là bất định, 10⁴ lần thử", state: "Cố ý chưa xây. unconfirmed_effect_intents() trả lời ý định nào thiếu kết cục — một phép đọc, không bao giờ là một quyết định. Tầng quyết định được nêu tên như phần việc có phạm vi riêng.", cite: "include/agentengine/rt/effect_journal.hpp:30" },
    { item: "Nhật ký hiệu ứng chưa được nối vào vòng lặp lượt", state: "journal_effect_intent()/journal_effect_outcome()/authorize_reexecution() KHÔNG có caller nào ngoài chính bài kiểm thử của chúng. run_rounds() suy ra và kiểm toán khóa idempotency ở mọi lời gọi, nhưng không ghi nhật ký gì. Host nào muốn kỷ luật của 019 §3 thì phải tự gọi nhật ký.", cite: "include/agentengine/rt/agent_session.hpp:1512" },
    { item: "Mất node, fencing, lease, epoch", state: "Không thứ nào tồn tại, theo quyết định. 019 §4 nay gọi đây là khoảng trống thật và vĩnh viễn; không có câu chuyện đa node và không có nguyên liệu so-sánh-rồi-đặt trong cả hai store.", cite: "019-Durability-and-Long-Running-Agents.md:73" },
    { item: "FileSessionStore không ghi nguyên tử", state: "save() cắt cụt rồi ghi đè tại chỗ — sập giữa chừng có thể để lại byte đứt dở. Log ghi thêm làm tốt hơn theo cấu trúc (chỉ mất phần đuôi dở, và phép đọc dừng gọn trước nó). Điều này nêu trong header, không phải phát hiện khi chạy thật.", cite: "include/agentengine/rt/session_store.hpp:149" },
    { item: "Chính sách ghim phiên bản khi triển khai của 019 §4", state: "Chưa hiện thực. AgentMetadata::agent_version là thật (ADR-044), nhưng không gì tra tới nó khi một run tiếp tục, nên chẳng có quyết định ghim-hay-chuyển nào để mà đưa ra.", cite: "include/agentengine/core/agent_registry.hpp:63" },
    { item: "Lưu giữ và thu gom rác của 019 §5", state: "Không chính sách lưu giữ, không thu gom rác, không thời hạn ân hạn ở đâu cả — header của project manifest nói đúng như vậy cho đường lưu trữ.", cite: "include/agentengine/rt/project_manifest.hpp:242" },
  ],
};

// ---- code snippets -------------------------------------------------------------------------------

export const checkpointSnippet = `// rt/agent_session.hpp -- the whole durable record, and how a host writes it.
struct AgentSessionRecord {
    std::string session_id;
    std::string principal_id;
    std::string principal_tenant_id;
    bool deleted = false;
    std::uint64_t run_counter = 0;
    std::uint64_t turn_index = 0;
    std::vector<Interaction> open_interactions;   // the only non-scalar field
};

// The in-flight guard that replaced quark::FenceToken: the SAME AsyncMutex
// start_run()/resolve_interaction() hold, acquired for the whole read.
task<AgentSessionRecord> snapshot_record() {
    AsyncMutex::Guard guard = co_await session_mutex_.lock();
    co_return to_record();
}

// Cadence is a pure predicate; the turn counter belongs to the caller,
// because AgentSession has no ambient Store access (I2).
template <std::uint32_t EveryNTurns> requires(EveryNTurns >= 1)
struct CheckpointCadence {
    static constexpr bool due(std::uint64_t turns_since_last) noexcept {
        return turns_since_last >= EveryNTurns;
    }
};

InMemorySessionStore store;                       // or FileSessionStore{root}
bool wrote = *co_await checkpoint_if_due<CheckpointCadence<3>>(session, store, turns);`;

export const minimalCheckpointSnippet = `// Common case: checkpoint every turn, no cadence math, in-memory to start.
InMemorySessionStore store;
co_await checkpoint_if_due<CheckpointCadence<1>>(session, store, /*turns_since_last=*/1);
// Same call once a restart needs to survive -- only the store type changes.
// FileSessionStore store{root};`;

export const deleteSnippet = `// rt/agent_session.hpp -- 005 §6's "hard removal, with a completion receipt".
struct SessionDeletionReceipt {
    std::string session_id;
    bool        durable_record_removed = false;
    bool        in_process_state_cleared = false;   // either half can fail alone
};

task<result<SessionDeletionReceipt>> delete_session(auto& session, auto& store) {
    SessionDeletionReceipt receipt{};
    receipt.session_id = session.session_id();

    AgentSessionRecord tombstone{};                 // a tombstone, not a remove()
    tombstone.session_id = receipt.session_id;
    tombstone.deleted    = true;
    if (auto saved = store.save(receipt.session_id,
                                encode_agent_session_record(tombstone)); !saved)
        co_return std::unexpected(saved.error());   // fail before clearing anything
    receipt.durable_record_removed = true;

    co_await session.clear_in_process_state_locked();
    receipt.in_process_state_cleared = true;
    co_return receipt;
}

// Read side: a tombstoned record is indistinguishable from one that never existed.
auto found = load_agent_session_snapshot(store, "s-1");   // -> std::nullopt`;

export const idempotencySnippet = `// core/tool_pipeline.hpp -- 019 §3's four inputs, and nothing else.
struct IdempotencyKey {
    std::string   run_id;
    std::uint64_t turn_index = 0;
    std::uint64_t call_index = 0;
    std::uint64_t argument_digest = 0;

    std::string to_string() const;   // "run:1:0:0:7356067401319992899"
};

IdempotencyKey derive_idempotency_key(EffectContext const& ctx,
                                      std::uint64_t call_index,
                                      json::Value const& arguments) {
    return IdempotencyKey{ctx.run_id, ctx.turn_index, call_index,
                          argument_digest(json::dump(arguments))};
}

// 019 §6's rewind rule. operator_acknowledged is an explicit host/human bool --
// never invented ambiently, never derived from model output (I3).
result<void> authorize_reexecution(effect_class cls, bool operator_acknowledged) {
    switch (cls) {
        case effect_class::pure:
        case effect_class::idempotent: return {};
        case effect_class::at_most_once:
            if (!operator_acknowledged)
                return std::unexpected(error{failure_class::policy,
                    "at-most-once effect requires explicit operator acknowledgement",
                    "effect.reexecution_requires_ack"});
            return {};
    }
}`;

export const journalSnippet = `// rt/effect_journal.hpp -- intent before, outcome after, both append-only.
struct EffectJournalEntry {
    std::string          idempotency_key;   // the dedup identity
    std::string          tool_name;
    std::string          call_id;
    effect_journal_phase phase = effect_journal_phase::intent;
    bool                 outcome_ok = false;      // only meaningful for outcome
};

LogId effect_journal_log_id(std::string_view session_id) {
    return std::string(session_id) + ":effect_journal";   // never collides with
}                                                          // the session's own slot

// On resume: which intents have no matching outcome, in commit order.
// A READ, not a decision -- deciding what to do about one is 019 §7 G6's own,
// separately-scoped work, deliberately not built here.
template <AppendLogStore StoreT>
result<std::vector<EffectJournalEntry>> unconfirmed_effect_intents(
    StoreT const& store, std::string_view session_id);`;

export const standingEffectSnippet = `// core/standing_effect.hpp -- one handle shape, three declared producers.
struct StandingEffect {
    std::string          handle_id;
    std::string          session_id;
    std::string          principal_id;   // 019 §2 G8: cross-principal cancel denial
    std::string          run_id;         // the run that ASKED for the work
    standing_effect_kind kind = standing_effect_kind::background_task;
    std::string          label;
    std::optional<std::chrono::steady_clock::time_point> fire_at;  // schedule_wakeup
};

// Authorized synchronously; only step 8 onward detaches.
auto effect = session.start_background_task(table, request, approve);
// -> tool.not_backgroundable / tool.state_capturing_not_backgroundable
//    tool.capability_not_held / tool.background_capacity_exceeded / approval denied

// A timer with no timer: the host reads the clock and asks what is due.
auto armed = session.schedule_wakeup(std::chrono::minutes{30}, "follow-up", now);
for (auto const& due : session.due_standing_effects(clock_now())) { /* host resumes */ }

// NOT durable: StandingEffect is in-memory only and is not an AgentSessionRecord field.`;
