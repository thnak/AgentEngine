// api/durability.html — 019's durability surface as it actually exists in this tree after ADR-037.
//
// House rule for this page (same as apiContent.ts's own banner): never blur "real and tested" with
// "a Reviewed RFC describes this but nothing implements it yet". 019 is the RFC whose ground moved
// most when ADR-037 removed Quark — no mailbox, no FenceToken, no ReminderService — so several
// sections below deliberately end on what is narrower than the RFC asks for, and the last section
// is nothing but that. Every claim carries a file:line citation.

import { SITE_BASE } from "../data/content";
import { gh } from "../data/apiContent";
import {
  checkpointFields,
  checkpointSnippet,
  deleteSnippet,
  durabilityEntries,
  durabilityGaps,
  effectClassRows,
  idempotencySnippet,
  journalSnippet,
  minimalCheckpointSnippet,
  proofRows,
  standingEffectSnippet,
  wakeRows,
  workedFlowStages,
} from "../data/durabilityContent";
import { useLang } from "../i18n/LanguageContext";
import type { Lang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { DuEffectJournalDiagram } from "./DuEffectJournalDiagram";
import { DuWakeDiagram } from "./DuWakeDiagram";
import { RevealGroup, RevealItem } from "./Reveal";

function entryById(lang: Lang, id: string) {
  const e = durabilityEntries[lang].find((entry) => entry.id === id);
  if (!e) throw new Error(`missing durability entry: ${id}`);
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

/** A `path/file.hpp:LINE` locator rendered as a link to the file itself. */
function Cite({ cite }: { cite: string }) {
  const path = cite.split(":")[0];
  return (
    <a
      className="api-cite"
      href={gh(path)}
      target="_blank"
      rel="noreferrer"
      style={{ borderTop: "none", paddingTop: 0, marginTop: 10, display: "block" }}
    >
      {cite}
    </a>
  );
}

/** The proofs table links the test file named first in each row. */
function TestCite({ label }: { label: string }) {
  const file = label.split(" ")[0];
  return (
    <a className="api-cite" href={gh(`tests/${file}`)} target="_blank" rel="noreferrer" style={{ borderTop: "none", paddingTop: 0, marginTop: 0, display: "block" }}>
      {label}
    </a>
  );
}

function StageMono({ text }: { text: string }) {
  return (
    <pre
      style={{
        margin: 0,
        padding: "12px 14px",
        borderRadius: "var(--radius-sm)",
        background: "var(--panel)",
        border: "1px solid var(--border)",
        color: "var(--text-dim)",
        fontFamily: "var(--font-mono)",
        fontSize: "0.76rem",
        lineHeight: 1.7,
        overflowX: "auto",
      }}
    >
      {text}
    </pre>
  );
}

const copy = {
  en: {
    eyebrow: "019 — Durability and Long-Running Agents",
    headingPrefix: "Runs that outlive the process,",
    headingHighlight: "and effects that don't repeat twice",
    intro: (
      <>
        A run is supposed to last hours or weeks, survive a restart, cost nothing while it waits,
        and never double-apply an external effect. In this tree that decomposes into four
        genuinely separate mechanisms — a small JSON checkpoint record, a suspension shape with no
        thread behind it, a deterministic idempotency key, and an append-only effect journal —
        plus a short, honest list of the things 019 asks for that nobody has built yet. ADR-037
        removed the actor engine this RFC was originally written against, so several of its own
        sentences are now out of date with the code; where they disagree, this page follows the
        code and says which sentence is stale.
      </>
    ),
    stat1: "7",
    stat1Label: "fields in a session checkpoint",
    stat2: "4",
    stat2Label: "inputs to an idempotency key",
    stat3: "3 / 6",
    stat3Label: "019 §2 wake rows with a real producer",

    s1Eyebrow: "rt/agent_session.hpp — AgentSessionRecord",
    s1Heading: "What a checkpoint actually carries",
    s1Body: (
      <>
        A checkpoint is not the conversation. It is a flat, seven-field bookkeeping record, JSON
        encoded by hand and handed to a host-supplied <code>rt::SessionStore</code> as opaque
        bytes. What it leaves out is more interesting than what it keeps — and every omission is
        named in the header, with a reason, rather than discovered later.
      </>
    ),
    colField: "Field",
    colCarried: "In the record",
    colWhy: "Why",
    carriedYes: "carried",
    carriedNo: "not carried",
    s1RecoLabel: "Recommended default",
    s1RecoBody: (
      <>
        Most agents don't need to reason about cadence or pick a store up front.{" "}
        <code>InMemorySessionStore</code> plus <code>CheckpointCadence&lt;1&gt;</code> — checkpoint
        after every turn — is the common case; the cost is one extra write per turn, worth paying
        until there's a measured reason to widen it. <code>checkpoint_if_due()</code> is the same
        call whether the store is in-memory or <code>FileSessionStore</code>, so moving to a store
        that survives a restart later is a type swap, not a rewrite.
      </>
    ),
    s1Note: (
      <>
        <strong>Two stores, two different durability stories.</strong>{" "}
        <code>FileSessionStore::save()</code> truncates and rewrites the whole file — a crash
        mid-write can leave torn bytes, stated plainly in the header rather than assumed away.{" "}
        <code>FileAppendLogStore</code> (which the effect journal uses) is structurally safer:
        every prior record survives regardless, only an unfinished trailing record can be lost,
        and <code>read_from()</code> stops cleanly before a torn tail instead of failing.
      </>
    ),

    s2Eyebrow: "delete_session() — 005 §6",
    s2Heading: "Delete writes a tombstone; the receipt names which half landed",
    s2Body: (
      <>
        Deleting a session is two independent operations — a durable write and an in-process
        clear — and either can fail on its own. That is exactly why the return value is a receipt
        with a flag for each, rather than a bare <code>result&lt;void&gt;</code>.
      </>
    ),
    s2Note: (
      <>
        <strong>Restore is deliberately narrow.</strong>{" "}
        <code>restore_from_record()</code> reinstates identity, <code>run_counter</code>,{" "}
        <code>turn_index</code> and open interactions, and re-derives <code>last_run_id</code>{" "}
        from the restored counter — so the next <code>start_run()</code> continues the original
        sequence instead of re-minting a run id that already happened. It does not restore the
        conversation, because the record never carried one.
      </>
    ),

    s3Eyebrow: "019 §2 — suspension",
    s3Heading: "A suspended run holds no thread, because there is no thread to hold",
    s3Body: (
      <>
        Before ADR-037 a session was an actor with a mailbox, and "suspended" meant an unanswered{" "}
        <code>Ask</code> sitting in a runtime. Now <code>run_rounds()</code> simply{" "}
        <em>returns</em> when a round suspends, carrying a sentinel error code, and what is left
        behind is an <code>Interaction</code> record. That makes 019 §2's "no activation, no
        sandbox, no connection, no thread" closer to true by construction than it ever was — and
        it means nothing inside AgentEngine can wake a run by itself.
      </>
    ),
    colCondition: "019 §2 wake row",
    colMechanism: "What actually exists here",
    s3Note: (
      <>
        <strong>No self-firing timer, on purpose.</strong> A design where{" "}
        <code>AgentSession</code> owns a background <code>std::jthread</code> was considered and
        rejected (ADR-053) for the same reason ADR-037 removed Quark's ambient background
        activity in the first place. <code>schedule_wakeup()</code> arms a{" "}
        <code>fire_at</code> from a <em>caller-supplied</em> <code>now</code> — never an ambient
        clock read (I5) — and a host polls <code>due_standing_effects(now)</code>. Deciding how
        often to poll is the host's problem, and a due entry is not auto-cleared.
      </>
    ),

    s4Eyebrow: "019 §3 — exactly-once effects",
    s4Heading: "A key that survives a restart, and a journal that admits what it doesn't know",
    s4Body: (
      <>
        The key is derived from exactly four inputs and nothing else — no wall clock, no
        randomness, no process identity — which is the whole reason it can be recomputed
        identically after a crash. <code>invoke_tool()</code> derives it unconditionally on every
        call and stamps it onto the audit record, so a repeat is recognizable as a repeat.
      </>
    ),
    colClass: "effect_class",
    colOnReexec: "On re-execution",
    colNote: "Note",
    s4Note: (
      <>
        <strong>The journal is real, tested — and not yet wired into the turn loop.</strong>{" "}
        <code>journal_effect_intent()</code>, <code>journal_effect_outcome()</code> and{" "}
        <code>authorize_reexecution()</code> have no caller anywhere outside their own tests:{" "}
        <code>run_rounds()</code> derives and audits the idempotency key on every tool call, but
        journals nothing. A host that wants 019 §3's discipline today has to call the journal
        around its own effects. Said plainly rather than left to be discovered.
      </>
    ),

    s5Eyebrow: "tests/test_effect_reexecution.cpp",
    s5Heading: "One at-most-once effect, interrupted, and refused a second time",
    s5Body: (
      <>
        This is the whole mechanism on one page, with the real key and the real journal entry at
        each step. The interruption in step 03 is where the test stops and the design continues:
        the replay and the refusal are proven against a manually re-issued call, not a fault
        injector, and this page does not claim otherwise.
      </>
    ),

    s6Eyebrow: "core/interaction.hpp · ADR-029 · ADR-057",
    s6Heading: "A run that stops for a human — the persistence half",
    s6Body: (
      <>
        <code>Interaction</code> is the one correlation record behind every kind of waiting:{" "}
        <code>input</code>, <code>auth</code>, <code>approval</code> and{" "}
        <code>codeact_ask</code>. It is all scalars and strings, so it round-trips through a
        checkpoint trivially — and it is the only non-scalar field the record carries. The
        approval flow's mechanics are on the{" "}
        <a href={`${SITE_BASE}/api/runtime.html`}>AgentSession page</a>; what follows is only
        what happens to it across a restart.
      </>
    ),
    step1Title: "The round suspends and an Interaction opens",
    step1Body: "run_rounds() returns with the kSuspendedForApproval sentinel before the gated tool's invoke() is ever reached. A second start_run() is refused with run.approval_pending — no new run id is minted.",
    step2Title: "The next checkpoint carries the interaction",
    step2Body: "open_interactions is the record's only non-scalar field, encoded by rt/interaction_codec.hpp. Restoring into a fresh instance recovers both the ids and each reason tag.",
    step3Title: "The process restarts",
    step3Body: "Identity, run_counter, turn_index and the open interactions come back. history_ does not — the record never carried it.",
    step4Title: "…and the resume cannot complete",
    step4Body: "resolve_interaction() requires history_'s tail to still be the exact assistant tool-call message that suspended; on a restored session it isn't, so the call fails session.resolve_interaction.stale. ADR-029 named this residual itself: the Interaction survives the restart, the resumable round does not.",
    s6Note: (
      <>
        <strong>The CodeAct variant pays a different price.</strong> An{" "}
        <code>agent.ask()</code> suspension resumes by <em>replaying</em> the stored script
        against the answers so far (ADR-057's abort-and-replay). A test proves what that costs:
        a mediated file write standing before the ask runs twice in total — <code>"XX"</code>,
        not <code>"X"</code>. The replay state itself is not checkpointed at all.
      </>
    ),

    s7Eyebrow: "006 §6b — StandingEffect",
    s7Heading: "Background work: authorized synchronously, executed detached",
    s7Body: (
      <>
        Everything that can refuse a background task refuses it on the calling thread, before any
        thread is spawned: an undeclared <code>Backgroundable</code> tool, a session-state-capturing
        tool, a missing capability, an exhausted <code>Background&lt;max_concurrent&gt;</code>{" "}
        grant, or a denied approval. Only step 8 onward detaches — and the detached copy of{" "}
        <code>EffectContext</code> has its <code>report_progress</code> callback structurally
        reset, so a background tool can never reach back into the originating session's event
        bookkeeping from a foreign thread.
      </>
    ),
    s7Note: (
      <>
        <strong>Cancellation is bookkeeping, and it checks the principal.</strong>{" "}
        <code>cancel_standing_effect(handle_id, caller_principal)</code> refuses a caller whose
        principal id doesn't match the effect's owner — the cross-principal denial{" "}
        <code>standing_effect.hpp</code> labels as 019 §2's own rule — and otherwise erases the
        entry — it does <em>not</em> interrupt an already-running native <code>invoke()</code>,
        because nothing in 006 §6b names a mechanism that could. A late completion for a
        cancelled handle finds nothing to resolve and no-ops. And none of this is durable:{" "}
        <code>StandingEffect</code> is in-memory only, so a wakeup armed just before a restart is
        simply gone.
      </>
    ),

    s8Eyebrow: "019 §4 — recovery",
    s8Heading: "Poison runs, node loss, deploys",
    s8Body: "Restart on the same node is the one recovery path with machinery behind it. The other three items in 019 §4 are in very different states, and rounding them up to \"handled\" would be the exact kind of blur this site's citation rule exists to prevent.",
    card1Title: "Poison runs — a policy without a home",
    card1Body: "019 §4 asks for a bounded retry with state preserved. The behaviour is proven — an always-failing client is quarantined at exactly the bound, history intact, session identity untouched — but PoisonRunPolicy<N> is not in any shipping header; it survives only as a dependency-free copy inside the test. Quarantining is host bookkeeping: it never calls into the session's delete or clear paths.",
    card2Title: "Node loss — not handled, by decision",
    card2Body: "Quark's FenceToken prevented two activations of one session. ADR-037 removed it and nothing replaced it: there is no fence, epoch or lease anywhere in this tree, no cross-process locking in either store, and five node-loss/fencing test files were retired as an accepted permanent gap. What survives is an in-process FIFO AsyncMutex — enough to make a snapshot safe against a concurrent turn, and nothing more.",
    card3Title: "Deploys — the version pin is unbuilt",
    card3Body: "019 §4 wants a version-skew policy defaulting to pin, so a resumed run doesn't silently change behaviour under new instructions. AgentMetadata::agent_version is real (ADR-044), but nothing consults it when a run resumes, so there is no pin-or-migrate decision being made at all.",

    s9Eyebrow: "tests/ — deterministic, offline",
    s9Heading: "The proofs, and exactly how far each one reaches",
    s9Body: "There is no test framework here: every file is a standalone int main() with a local check(cond, label), so the label is the claim. The third column is the honest scope line — several of these are narrower than the 019 §7 gate they sit closest to.",
    colTest: "Test",
    colAsserts: "What it asserts",
    colScope: "How far it reaches",

    s10Eyebrow: "019 §7 — the promotion gate",
    s10Heading: "What is still missing, stated plainly",
    s10Body: "One of 019's six gates is met, for workflows rather than sessions. Here is the rest of the scoreboard, including two places where 019's own text now contradicts the code.",
    colItem: "019 asks for",
    colState: "Where this tree stands",
    s10Note: (
      <>
        <strong>Two stale sentences in 019 itself.</strong> §2's Timer/schedule row and §7's G4
        both still describe durable reminders as having "carried over intact" into{" "}
        <code>rt::</code>. They did not: ADR-037 deleted Quark's <code>ReminderService</code>,{" "}
        <code>rt::</code> has never had any timer or delay primitive, and{" "}
        <code>schedule_wakeup</code> is host-polled by construction. Per this project's own rule
        the spec wins when code and spec disagree — which here means 019 needs an amendment, not
        that the timer secretly exists.
      </>
    ),
  },
  vi: {
    eyebrow: "019 — Tính bền vững và Agent chạy dài",
    headingPrefix: "Những run sống lâu hơn tiến trình,",
    headingHighlight: "và những hiệu ứng không lặp lại lần hai",
    intro: (
      <>
        Một run lẽ ra phải kéo dài hàng giờ hay hàng tuần, sống sót qua khởi động lại, gần như
        không tốn gì trong lúc chờ, và không bao giờ áp dụng hai lần một hiệu ứng ra bên ngoài.
        Trong cây mã này, điều đó tách thành bốn cơ chế thực sự riêng biệt — một bản ghi
        checkpoint JSON nhỏ, một hình thái treo không có luồng nào đứng sau, một khóa idempotency
        tất định, và một nhật ký hiệu ứng chỉ ghi thêm — cộng thêm một danh sách ngắn và trung
        thực những gì 019 yêu cầu mà chưa ai xây. ADR-037 đã bỏ đi bộ máy actor mà RFC này vốn
        viết dựa trên, nên vài câu chữ của chính nó nay đã lệch so với mã; ở đâu hai bên bất
        đồng, trang này theo mã và nói rõ câu nào đã cũ.
      </>
    ),
    stat1: "7",
    stat1Label: "trường trong một checkpoint session",
    stat2: "4",
    stat2Label: "đầu vào của một khóa idempotency",
    stat3: "3 / 6",
    stat3Label: "dòng wake của 019 §2 có nguồn phát thật",

    s1Eyebrow: "rt/agent_session.hpp — AgentSessionRecord",
    s1Heading: "Một checkpoint thực sự mang theo những gì",
    s1Body: (
      <>
        Checkpoint không phải cuộc hội thoại. Nó là một bản ghi sổ sách phẳng gồm bảy trường, mã
        hóa JSON bằng tay rồi trao cho một <code>rt::SessionStore</code> do host cung cấp dưới
        dạng byte mờ. Thứ nó bỏ lại còn thú vị hơn thứ nó giữ — và mỗi phần bỏ lại đều được nêu
        tên ngay trong header, kèm lý do, chứ không phải để người ta phát hiện về sau.
      </>
    ),
    colField: "Trường",
    colCarried: "Trong bản ghi",
    colWhy: "Vì sao",
    carriedYes: "có mang",
    carriedNo: "không mang",
    s1RecoLabel: "Mặc định khuyến nghị",
    s1RecoBody: (
      <>
        Phần lớn agent không cần cân nhắc nhịp checkpoint hay chọn store ngay từ đầu.{" "}
        <code>InMemorySessionStore</code> cùng <code>CheckpointCadence&lt;1&gt;</code> — checkpoint
        sau mỗi lượt — là trường hợp phổ biến; cái giá là thêm một lần ghi mỗi lượt, đáng trả cho
        tới khi có lý do đo được để nới rộng nó. <code>checkpoint_if_due()</code> là cùng một lời
        gọi dù store là in-memory hay <code>FileSessionStore</code>, nên chuyển sang một store sống
        sót qua khởi động lại về sau chỉ là đổi kiểu, không phải viết lại.
      </>
    ),
    s1Note: (
      <>
        <strong>Hai store, hai câu chuyện bền vững khác nhau.</strong>{" "}
        <code>FileSessionStore::save()</code> cắt cụt rồi ghi lại cả tệp — sập giữa chừng có thể
        để lại byte đứt dở, điều này được nói thẳng trong header chứ không lờ đi.{" "}
        <code>FileAppendLogStore</code> (thứ mà nhật ký hiệu ứng dùng) an toàn hơn về cấu trúc:
        mọi bản ghi trước đó vẫn nguyên vẹn dù thế nào, chỉ bản ghi cuối chưa xong mới có thể
        mất, và <code>read_from()</code> dừng gọn trước phần đuôi đứt dở thay vì báo lỗi.
      </>
    ),

    s2Eyebrow: "delete_session() — 005 §6",
    s2Heading: "Xóa thì ghi một bia mộ; biên nhận nói rõ nửa nào đã xong",
    s2Body: (
      <>
        Xóa một session là hai thao tác độc lập — một lần ghi bền vững và một lần dọn trong tiến
        trình — và mỗi cái có thể hỏng riêng. Đó chính là lý do giá trị trả về là một biên nhận
        có cờ cho từng nửa, chứ không phải một <code>result&lt;void&gt;</code> trơ trọi.
      </>
    ),
    s2Note: (
      <>
        <strong>Việc khôi phục hẹp một cách có chủ đích.</strong>{" "}
        <code>restore_from_record()</code> khôi phục danh tính, <code>run_counter</code>,{" "}
        <code>turn_index</code> và các interaction đang mở, rồi suy lại <code>last_run_id</code>{" "}
        từ bộ đếm vừa khôi phục — nhờ vậy <code>start_run()</code> kế tiếp đi tiếp dãy gốc thay
        vì mint lại một run id đã từng xảy ra. Nó không khôi phục cuộc hội thoại, vì bản ghi
        chưa từng mang theo cái đó.
      </>
    ),

    s3Eyebrow: "019 §2 — treo",
    s3Heading: "Một run đang treo không giữ luồng nào, vì chẳng có luồng nào để mà giữ",
    s3Body: (
      <>
        Trước ADR-037, một session là một actor có hòm thư, và "treo" nghĩa là một{" "}
        <code>Ask</code> chưa được trả lời nằm trong runtime. Giờ đây <code>run_rounds()</code>{" "}
        đơn giản là <em>trả về</em> khi một vòng treo lại, mang theo một mã lỗi sentinel, và thứ
        còn lại là một bản ghi <code>Interaction</code>. Điều đó khiến "không activation, không
        sandbox, không kết nối, không luồng" của 019 §2 đúng theo cấu trúc hơn bao giờ hết — và
        cũng có nghĩa là không thứ gì bên trong AgentEngine tự đánh thức được một run.
      </>
    ),
    colCondition: "Dòng wake của 019 §2",
    colMechanism: "Thực tế có gì ở đây",
    s3Note: (
      <>
        <strong>Không có bộ đếm giờ tự kích hoạt, và đó là chủ ý.</strong> Phương án để{" "}
        <code>AgentSession</code> sở hữu một <code>std::jthread</code> nền đã được cân nhắc rồi
        bác bỏ (ADR-053), đúng vì lý do ADR-037 đã bỏ hoạt động nền ngầm của Quark ngay từ đầu.{" "}
        <code>schedule_wakeup()</code> đặt <code>fire_at</code> từ tham số <code>now</code>{" "}
        <em>do caller cung cấp</em> — không bao giờ đọc đồng hồ ngầm (I5) — và host phải hỏi{" "}
        <code>due_standing_effects(now)</code>. Hỏi bao lâu một lần là việc của host, và một mục
        đã tới hạn không tự bị xóa.
      </>
    ),

    s4Eyebrow: "019 §3 — hiệu ứng đúng một lần",
    s4Heading: "Một khóa sống sót qua khởi động lại, và một nhật ký thừa nhận điều nó không biết",
    s4Body: (
      <>
        Khóa được suy ra từ đúng bốn đầu vào và không gì khác — không đồng hồ tường, không ngẫu
        nhiên, không danh tính tiến trình — và đó chính là lý do nó tính lại được y hệt sau một
        cú sập. <code>invoke_tool()</code> suy ra khóa vô điều kiện ở mọi lời gọi và đóng dấu vào
        bản ghi kiểm toán, nhờ vậy một lần lặp lại vẫn nhận ra được là lặp lại.
      </>
    ),
    colClass: "effect_class",
    colOnReexec: "Khi chạy lại",
    colNote: "Ghi chú",
    s4Note: (
      <>
        <strong>Nhật ký là thật, đã kiểm thử — và chưa được nối vào vòng lặp lượt.</strong>{" "}
        <code>journal_effect_intent()</code>, <code>journal_effect_outcome()</code> và{" "}
        <code>authorize_reexecution()</code> không có caller nào ngoài chính bài kiểm thử của
        chúng: <code>run_rounds()</code> suy ra và kiểm toán khóa idempotency ở mọi lời gọi tool,
        nhưng không ghi nhật ký gì. Một host muốn kỷ luật của 019 §3 ngay hôm nay thì phải tự gọi
        nhật ký quanh hiệu ứng của mình. Nói thẳng ra, thay vì để người dùng tự phát hiện.
      </>
    ),

    s5Eyebrow: "tests/test_effect_reexecution.cpp",
    s5Heading: "Một hiệu ứng at-most-once, bị ngắt, và bị từ chối ở lần thứ hai",
    s5Body: (
      <>
        Đây là toàn bộ cơ chế gói trong một trang, với khóa thật và bản ghi nhật ký thật ở từng
        bước. Sự gián đoạn ở bước 03 là chỗ bài kiểm thử dừng lại còn thiết kế thì đi tiếp: việc
        phát lại và việc từ chối được chứng minh trên một lời gọi phát lại thủ công, không phải
        bằng bộ tiêm lỗi, và trang này không tuyên bố khác đi.
      </>
    ),

    s6Eyebrow: "core/interaction.hpp · ADR-029 · ADR-057",
    s6Heading: "Một run dừng lại vì con người — nửa thuộc về tính bền vững",
    s6Body: (
      <>
        <code>Interaction</code> là bản ghi tương quan duy nhất đứng sau mọi kiểu chờ:{" "}
        <code>input</code>, <code>auth</code>, <code>approval</code> và{" "}
        <code>codeact_ask</code>. Nó toàn số và chuỗi nên đi qua checkpoint rất dễ — và là trường
        không vô hướng duy nhất mà bản ghi mang theo. Cơ chế của luồng phê duyệt nằm ở{" "}
        <a href={`${SITE_BASE}/api/runtime.html`}>trang AgentSession</a>; phần dưới đây chỉ nói
        điều gì xảy ra với nó qua một lần khởi động lại.
      </>
    ),
    step1Title: "Vòng chạy treo lại và một Interaction mở ra",
    step1Body: "run_rounds() trả về với sentinel kSuspendedForApproval trước khi invoke() của tool bị chặn kịp được chạm tới. Một start_run() thứ hai bị từ chối với run.approval_pending — không run id mới nào được mint.",
    step2Title: "Checkpoint kế tiếp mang theo interaction đó",
    step2Body: "open_interactions là trường không vô hướng duy nhất của bản ghi, mã hóa bởi rt/interaction_codec.hpp. Khôi phục vào một thể hiện mới lấy lại được cả id lẫn thẻ lý do của từng cái.",
    step3Title: "Tiến trình khởi động lại",
    step3Body: "Danh tính, run_counter, turn_index và các interaction đang mở quay về. history_ thì không — bản ghi chưa từng mang nó.",
    step4Title: "…và việc tiếp tục không thể hoàn tất",
    step4Body: "resolve_interaction() đòi phần đuôi của history_ vẫn phải là đúng thông điệp gọi tool của assistant lúc treo; trên một session vừa khôi phục thì không phải vậy, nên lời gọi hỏng với session.resolve_interaction.stale. Chính ADR-029 đã nêu tên phần dư này: Interaction sống sót qua khởi động lại, còn vòng chạy có thể tiếp tục thì không.",
    s6Note: (
      <>
        <strong>Biến thể CodeAct trả một cái giá khác.</strong> Một lần treo do{" "}
        <code>agent.ask()</code> tiếp tục bằng cách <em>phát lại</em> đoạn script đã lưu cùng các
        câu trả lời đến lúc đó (kiểu hủy-rồi-phát-lại của ADR-057). Một bài kiểm thử chứng minh
        cái giá ấy: một lần ghi tệp có trung gian đứng trước câu hỏi chạy tổng cộng hai lần —{" "}
        <code>"XX"</code>, không phải <code>"X"</code>. Bản thân trạng thái phát lại thì hoàn
        toàn không được checkpoint.
      </>
    ),

    s7Eyebrow: "006 §6b — StandingEffect",
    s7Heading: "Việc chạy nền: cấp phép đồng bộ, thực thi tách rời",
    s7Body: (
      <>
        Mọi thứ có thể từ chối một tác vụ nền đều từ chối ngay trên luồng gọi, trước khi có luồng
        nào được sinh ra: một tool không khai báo <code>Backgroundable</code>, một tool bắt trạng
        thái session, thiếu capability, hết hạn mức{" "}
        <code>Background&lt;max_concurrent&gt;</code>, hay một phê duyệt bị từ chối. Chỉ từ bước
        8 trở đi mới tách rời — và bản sao <code>EffectContext</code> đi theo luồng tách rời bị
        đặt lại callback <code>report_progress</code> ngay từ cấu trúc, nên một tool chạy nền
        không bao giờ với ngược vào sổ sách sự kiện của session gốc từ một luồng lạ.
      </>
    ),
    s7Note: (
      <>
        <strong>Hủy chỉ là sổ sách, và nó kiểm tra principal.</strong>{" "}
        <code>cancel_standing_effect(handle_id, caller_principal)</code> từ chối caller có
        principal id không khớp chủ sở hữu của hiệu ứng — phần từ chối chéo principal mà{" "}
        <code>standing_effect.hpp</code> gán cho luật của 019 §2 — còn lại thì xóa mục đó; nó{" "}
        <em>không</em> ngắt một <code>invoke()</code> gốc đang chạy, bởi 006 §6b không nêu tên cơ
        chế nào làm được vậy. Một kết quả tới muộn cho handle đã hủy sẽ không tìm thấy gì để giải
        quyết và lặng lẽ bỏ qua. Và không thứ nào ở đây bền vững:{" "}
        <code>StandingEffect</code> chỉ nằm trong bộ nhớ, nên một lần đánh thức trang bị ngay
        trước khởi động lại là mất hẳn.
      </>
    ),

    s8Eyebrow: "019 §4 — phục hồi",
    s8Heading: "Run độc, mất node, triển khai",
    s8Body: "Khởi động lại trên cùng một node là đường phục hồi duy nhất có bộ máy đứng sau. Ba mục còn lại của 019 §4 đang ở những tình trạng rất khác nhau, và làm tròn chúng thành \"đã xử lý\" đúng là kiểu nhập nhèm mà quy tắc trích dẫn của trang này sinh ra để ngăn.",
    card1Title: "Run độc — một chính sách không có nhà",
    card1Body: "019 §4 yêu cầu thử lại có chặn và giữ nguyên trạng thái. Hành vi đó đã được chứng minh — một client luôn hỏng bị cách ly đúng tại ngưỡng, lịch sử còn nguyên, danh tính session không bị đụng — nhưng PoisonRunPolicy<N> không nằm trong header xuất bản nào; nó chỉ còn sống như một bản sao không phụ thuộc bên trong bài kiểm thử. Việc cách ly là sổ sách của host: nó không bao giờ gọi vào đường xóa hay dọn của session.",
    card2Title: "Mất node — không xử lý, theo quyết định",
    card2Body: "FenceToken của Quark từng ngăn hai lần kích hoạt của cùng một session. ADR-037 bỏ nó và không gì thay thế: trong cây mã này không có fence, epoch hay lease nào, không store nào khóa liên tiến trình, và năm tệp kiểm thử về mất node/fencing đã bị rút như một khoảng trống vĩnh viễn được chấp nhận. Thứ còn lại là một AsyncMutex FIFO trong tiến trình — đủ để một snapshot an toàn trước một lượt đang chạy, và chỉ vậy thôi.",
    card3Title: "Triển khai — việc ghim phiên bản chưa xây",
    card3Body: "019 §4 muốn một chính sách lệch phiên bản mặc định là ghim, để một run tiếp tục không âm thầm đổi hành vi dưới bộ chỉ dẫn mới. AgentMetadata::agent_version là thật (ADR-044), nhưng không gì tra tới nó khi một run tiếp tục, nên thực tế chẳng có quyết định ghim-hay-chuyển nào được đưa ra cả.",

    s9Eyebrow: "tests/ — tất định, ngoại tuyến",
    s9Heading: "Các bằng chứng, và mỗi cái vươn xa tới đâu",
    s9Body: "Ở đây không có framework kiểm thử nào: mỗi tệp là một int main() độc lập với một hàm check(cond, label) cục bộ, nên cái nhãn chính là tuyên bố. Cột thứ ba là dòng phạm vi trung thực — vài bài hẹp hơn cổng 019 §7 mà chúng gần nhất.",
    colTest: "Bài kiểm thử",
    colAsserts: "Nó khẳng định điều gì",
    colScope: "Vươn xa tới đâu",

    s10Eyebrow: "019 §7 — cổng thăng hạng",
    s10Heading: "Những gì còn thiếu, nói thẳng ra",
    s10Body: "Một trong sáu cổng của 019 đã đạt, nhưng cho workflow chứ không phải session. Đây là phần còn lại của bảng điểm, gồm cả hai chỗ mà chính văn bản 019 nay mâu thuẫn với mã.",
    colItem: "019 yêu cầu",
    colState: "Cây mã này đang ở đâu",
    s10Note: (
      <>
        <strong>Hai câu đã cũ nằm ngay trong 019.</strong> Dòng Bộ đếm giờ/lịch ở §2 và cổng G4 ở
        §7 đều vẫn mô tả phần nhắc giờ bền vững là đã "chuyển nguyên vẹn" sang{" "}
        <code>rt::</code>. Không hề: ADR-037 đã xóa <code>ReminderService</code> của Quark,{" "}
        <code>rt::</code> chưa từng có nguyên liệu đếm giờ hay trì hoãn nào, và{" "}
        <code>schedule_wakeup</code> do host hỏi vòng theo đúng cấu trúc. Theo quy tắc của chính
        dự án, khi mã và đặc tả bất đồng thì đặc tả thắng — ở đây điều đó nghĩa là 019 cần một
        bản sửa đổi, chứ không phải bộ đếm giờ đang tồn tại đâu đó.
      </>
    ),
  },
} as const;

export function ApiDurabilityReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];

  return (
    <section className="section" id="durability">
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
          <div className="stat-row" style={{ marginTop: 26 }}>
            <div className="stat">
              <span className="num">{t.stat1}</span>
              <span className="label">{t.stat1Label}</span>
            </div>
            <div className="stat">
              <span className="num">{t.stat2}</span>
              <span className="label">{t.stat2Label}</span>
            </div>
            <div className="stat">
              <span className="num">{t.stat3}</span>
              <span className="label">{t.stat3Label}</span>
            </div>
          </div>
        </div>

        {/* ---- 1. What a checkpoint carries -------------------------------------------------- */}
        <section className="anchor-target" id="du-checkpoint">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s1Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
                <p>{t.s1Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <div className="gs-recommend">
                <span className="gs-recommend-label">{t.s1RecoLabel}</span>
                <p>{t.s1RecoBody}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <CodePanel filename="rt/agent_session.hpp">
                {highlightCpp(minimalCheckpointSnippet)}
              </CodePanel>
            </RevealItem>

            <RevealItem>
              <ApiTable
                columns={[t.colField, t.colCarried, t.colWhy]}
                templateColumns="1.1fr 0.55fr 2.2fr"
                rows={checkpointFields[lang].map((f) => [
                  <code>{f.field}</code>,
                  <span
                    className={`status-badge ${f.carried === "yes" ? "status-real" : "status-design"}`}
                  >
                    {f.carried === "yes" ? t.carriedYes : t.carriedNo}
                  </span>,
                  <>
                    {f.note}
                    <Cite cite={f.cite} />
                  </>,
                ])}
              />
            </RevealItem>

            <RevealItem>
              <CodePanel filename="rt/agent_session.hpp">{highlightCpp(checkpointSnippet)}</CodePanel>
            </RevealItem>

            <RevealItem>
              <p className="gs-note" style={{ marginTop: 20 }}>{t.s1Note}</p>
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-checkpoint" />
            </RevealItem>
          </RevealGroup>
        </section>

        {/* ---- 2. Delete, tombstone, receipt -------------------------------------------------- */}
        <section className="anchor-target" id="du-delete">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginTop: 56, marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s2Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
                <p>{t.s2Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <CodePanel filename="rt/agent_session.hpp">{highlightCpp(deleteSnippet)}</CodePanel>
            </RevealItem>

            <RevealItem>
              <p className="gs-note" style={{ marginTop: 20 }}>{t.s2Note}</p>
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-delete" />
            </RevealItem>
          </RevealGroup>
        </section>

        {/* ---- 3. Suspension and wake conditions ---------------------------------------------- */}
        <section className="anchor-target" id="du-suspension">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginTop: 56, marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s3Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
                <p>{t.s3Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <DuWakeDiagram />
            </RevealItem>

            <RevealItem>
              <div style={{ marginTop: 26 }}>
                <ApiTable
                  columns={[t.colCondition, t.colMechanism]}
                  templateColumns="0.8fr 2.4fr"
                  rows={wakeRows[lang].map((w) => [
                    <>
                      <strong>{w.condition}</strong>
                      <br />
                      <span
                        className={`status-badge ${w.status === "real" ? "status-real" : "status-design"}`}
                        style={{ marginTop: 8 }}
                      >
                        {w.status === "real" ? tu.statusRealTested : tu.statusDesignedNotBuilt}
                      </span>
                    </>,
                    <>
                      {w.mechanism}
                      <Cite cite={w.cite} />
                    </>,
                  ])}
                />
              </div>
            </RevealItem>

            <RevealItem>
              <p className="gs-note" style={{ marginTop: 20 }}>{t.s3Note}</p>
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-suspension" />
            </RevealItem>
          </RevealGroup>
        </section>

        {/* ---- 4. Idempotency key and effect journal ------------------------------------------ */}
        <section className="anchor-target" id="du-effects">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginTop: 56, marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s4Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
                <p>{t.s4Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <CodePanel filename="core/tool_pipeline.hpp">
                {highlightCpp(idempotencySnippet)}
              </CodePanel>
            </RevealItem>

            <RevealItem>
              <div style={{ marginTop: 22 }}>
                <ApiTable
                  columns={[t.colClass, t.colOnReexec, t.colNote]}
                  templateColumns="0.7fr 1fr 2fr"
                  rows={effectClassRows[lang].map((e) => [
                    <code>{e.cls}</code>,
                    e.onReexecution,
                    e.note,
                  ])}
                />
              </div>
            </RevealItem>

            <RevealItem>
              <CodePanel filename="rt/effect_journal.hpp">{highlightCpp(journalSnippet)}</CodePanel>
            </RevealItem>

            <RevealItem>
              <p
                className="gs-note"
                style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}
              >
                {t.s4Note}
              </p>
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-effects" />
            </RevealItem>
          </RevealGroup>
        </section>

        {/* ---- 5. One effect across a restart -------------------------------------------------- */}
        <section className="anchor-target" id="du-worked-flow">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginTop: 56, marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s5Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
                <p>{t.s5Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <DuEffectJournalDiagram />
            </RevealItem>

            <RevealItem>
              <div className="glass" style={{ marginTop: 26, padding: "6px 22px", borderRadius: "var(--radius-lg)" }}>
                {workedFlowStages[lang].map((s) => (
                  <div className="flow-stage" key={s.index}>
                    <span className="flow-stage-index">{s.index}</span>
                    <div>
                      <h4>{s.title}</h4>
                      <p>{s.body}</p>
                      <StageMono text={s.mono} />
                    </div>
                  </div>
                ))}
              </div>
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-worked-flow" />
            </RevealItem>
          </RevealGroup>
        </section>

        {/* ---- 6. Stopping for a human --------------------------------------------------------- */}
        <section className="anchor-target" id="du-interactions">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginTop: 56, marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s6Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
                <p>{t.s6Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <div className="ladder glass" style={{ padding: "6px 20px" }}>
                <div className="ladder-step">
                  <span className="ladder-index">01</span>
                  <div>
                    <h4>{t.step1Title}</h4>
                    <p>{t.step1Body}</p>
                  </div>
                </div>
                <div className="ladder-step">
                  <span className="ladder-index">02</span>
                  <div>
                    <h4>{t.step2Title}</h4>
                    <p>{t.step2Body}</p>
                  </div>
                </div>
                <div className="ladder-step">
                  <span className="ladder-index">03</span>
                  <div>
                    <h4>{t.step3Title}</h4>
                    <p>{t.step3Body}</p>
                  </div>
                </div>
                <div className="ladder-step is-current">
                  <span className="ladder-index">04</span>
                  <div>
                    <h4>{t.step4Title}</h4>
                    <p>{t.step4Body}</p>
                  </div>
                </div>
              </div>
            </RevealItem>

            <RevealItem>
              <p
                className="gs-note"
                style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}
              >
                {t.s6Note}
              </p>
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-interactions" />
            </RevealItem>
          </RevealGroup>
        </section>

        {/* ---- 7. Standing effects ------------------------------------------------------------- */}
        <section className="anchor-target" id="du-standing">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginTop: 56, marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s7Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s7Heading}</h3>
                <p>{t.s7Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <CodePanel filename="core/standing_effect.hpp">
                {highlightCpp(standingEffectSnippet)}
              </CodePanel>
            </RevealItem>

            <RevealItem>
              <p className="gs-note" style={{ marginTop: 20 }}>{t.s7Note}</p>
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-standing" />
            </RevealItem>
          </RevealGroup>
        </section>

        {/* ---- 8. Recovery --------------------------------------------------------------------- */}
        <section className="anchor-target" id="du-recovery">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginTop: 56, marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s8Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s8Heading}</h3>
                <p>{t.s8Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <div className="api-grid">
                <div className="api-card glass">
                  <div className="api-card-head">
                    <span className="api-tag">019 §4</span>
                    <span className="status-badge status-design">{tu.statusDesignedNotBuilt}</span>
                  </div>
                  <h4>{t.card1Title}</h4>
                  <p>{t.card1Body}</p>
                </div>
                <div className="api-card glass">
                  <div className="api-card-head">
                    <span className="api-tag">ADR-037</span>
                    <span className="status-badge status-design">{tu.statusDesignedNotBuilt}</span>
                  </div>
                  <h4>{t.card2Title}</h4>
                  <p>{t.card2Body}</p>
                </div>
                <div className="api-card glass">
                  <div className="api-card-head">
                    <span className="api-tag">ADR-044</span>
                    <span className="status-badge status-design">{tu.statusDesignedNotBuilt}</span>
                  </div>
                  <h4>{t.card3Title}</h4>
                  <p>{t.card3Body}</p>
                </div>
              </div>
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-recovery" />
            </RevealItem>
          </RevealGroup>
        </section>

        {/* ---- 9. The proofs -------------------------------------------------------------------- */}
        <section className="anchor-target" id="du-proofs">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginTop: 56, marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s9Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s9Heading}</h3>
                <p>{t.s9Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <ApiTable
                columns={[t.colTest, t.colAsserts, t.colScope]}
                templateColumns="1.1fr 2fr 1.4fr"
                rows={proofRows[lang].map((p) => [
                  <TestCite label={p.test} />,
                  p.asserts,
                  <span style={{ color: "var(--text-faint)" }}>{p.scope}</span>,
                ])}
              />
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-proofs" />
            </RevealItem>
          </RevealGroup>
        </section>

        {/* ---- 10. What is still missing --------------------------------------------------------- */}
        <section className="anchor-target" id="du-gaps">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginTop: 56, marginBottom: 22, maxWidth: 780 }}>
                <span className="eyebrow">{t.s10Eyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s10Heading}</h3>
                <p>{t.s10Body}</p>
              </div>
            </RevealItem>

            <RevealItem>
              <ApiTable
                columns={[t.colItem, t.colState]}
                templateColumns="1fr 1.7fr"
                rows={durabilityGaps[lang].map((g) => [
                  <strong>{g.item}</strong>,
                  <>
                    {g.state}
                    <Cite cite={g.cite} />
                  </>,
                ])}
              />
            </RevealItem>

            <RevealItem>
              <p
                className="gs-note"
                style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}
              >
                {t.s10Note}
              </p>
            </RevealItem>

            <RevealItem>
              <CiteLink id="du-gaps" />
            </RevealItem>
          </RevealGroup>
        </section>
      </div>
    </section>
  );
}
