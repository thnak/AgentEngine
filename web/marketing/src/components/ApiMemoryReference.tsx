import { gh } from "../data/apiContent";
import {
  memoryEntries,
  memoryFlowStages,
  memoryGapRows,
  memoryInjectionSnippet,
  memoryItemSnippet,
  memoryProofRows,
  memoryProviderDefaultSnippet,
  memoryProviderWiringSnippet,
  memoryRankSnippet,
  memorySourceRows,
  memoryStorageSnippet,
} from "../data/memoryContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { MemReadPathDiagram } from "./MemReadPathDiagram";
import { MemWritePathDiagram } from "./MemWritePathDiagram";
import { RevealGroup, RevealItem } from "./Reveal";
import type { Lang } from "../i18n/LanguageContext";

function entriesById(lang: Lang, ids: string[]) {
  return ids.map((id) => {
    const e = memoryEntries[lang].find((entry) => entry.id === id);
    if (!e) throw new Error(`missing memory entry: ${id}`);
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
    eyebrow: "029 — Memory System · 005 §5",
    headingPrefix: "Memory that",
    headingHighlight: "never launders authority",
    intro: (
      <>
        Memory here is not a wrapped vector database. It is what falls out of pointing two things
        this engine already owns at a new purpose: the worktree's content-addressed object store,
        and the <code>ContextProvider</code> seam. A <code>MemoryItem</code>'s identity is the
        digest of its own text, its storage is an ordinary blob at{" "}
        <code>&lt;kind&gt;/&lt;id&gt;</code> under a principal-scoped ref, its retrieval is
        arithmetic over stored fields with no model call, and its provenance is a trust signal the
        rendering layer respects and the permission layer structurally cannot be fooled by.
      </>
    ),

    s1Eyebrow: "core/memory.hpp — 029 §3",
    s1Heading: "MemoryItem: structured and provenanced, never an opaque blob of text",
    s1Body: (
      <>
        Four fields carry the whole design. <code>id</code> is the content digest, so identity is
        what the memory <em>is</em>, not a key someone handed it. <code>origin</code> records who
        or what produced it, in which run and turn. <code>salience</code> is the continuous 0..1
        weight 029 §7's decay model is meant to move. <code>write_seq</code> is the memory ref's
        own append-log sequence number, which is the only honest source of "recency" in a store
        whose <code>Ref</code> carries no history chain.
      </>
    ),
    sourceCols: ["memory_source", "Rendered label (ADR-046)", "What it means", "Who writes it today"],
    s1Note: (
      <>
        <strong>The rule that shapes everything below (029 §6).</strong> No{" "}
        <code>MemoryItem</code>, regardless of its <code>memory_source</code>, may satisfy a
        policy predicate that requires a user assertion. That is I3 applied to storage: memory is
        model-derived content the moment it passes through extraction, even though it is{" "}
        <em>stored</em> like first-class engine state. The engine does not enforce this by
        checking a field — it enforces it by making the field unreachable from the decision.
      </>
    ),

    s2Eyebrow: "core/memory.hpp + core/worktree.hpp — 029 §2",
    s2Heading: "Storage: a worktree, one scope level up — no second storage engine",
    s2Body: (
      <>
        A session worktree is <code>session:s-42</code>; a memory worktree is{" "}
        <code>principal:&lt;tenant&gt;:&lt;id&gt;</code>. Same <code>Ref</code>, same{" "}
        <code>Mount</code>, same <code>mount_read</code>/<code>mount_write</code>, same
        capability check — <code>memory.hpp</code> adds zero new worktree machinery. Memory
        therefore inherits content-addressed dedup, diffability and forkability for free, and
        "what memory exists" is answerable with an ordinary tree listing rather than a query
        language only the host understands.
      </>
    ),
    s2Note: (
      <>
        <strong>Both names are pure functions of the <code>Principal</code>, deliberately.</strong>{" "}
        <code>mount_read</code>/<code>mount_write</code> compare bare <code>mount_id</code> STRINGS
        with no knowledge of which principal either side belongs to. An earlier draft let the
        caller choose the mount id — which meant a plausible deployment (every principal's memory
        mounted under a shared literal id) would let a capability minted for principal A satisfy
        the check against a <code>Mount</code> built for principal B. That is the exact
        shared-index leakage class 029 §8/§9 names, arrived at by an API footgun rather than a
        hostile plugin.
      </>
    ),

    s3Eyebrow: "core/memory_provider.hpp — 029 §5, ADR-047",
    s3Heading: "The read path: rank, label, inject — and contribute a tool",
    s3Body: (
      <>
        <code>on_context()</code> takes the last user message as its query text, lists every stored
        item, scores each one, sorts, and keeps the top <code>max_injected</code> (default 3).
        Every input is stored structured data or the turn's own text: no clock, no network, no
        randomness. That is what makes byte-identical replay a testable claim rather than an
        aspiration.
      </>
    ),
    s3Note: (
      <>
        <strong>The forgery surface this had to close on its own (ADR-046).</strong> Retrieved
        content is tainted, model-influenced text — it can contain the literal marker bytes of a
        higher-trust label, whether by coincidence or by a deliberate attempt to make a reader
        believe a differently-sourced item follows. Every occurrence of the marker's open token
        inside <code>content</code> is broken with a zero-width space before the item's own real,
        structurally-emitted label is prepended, so the exact unbroken marker can only ever appear
        where the renderer itself put it. The label carries no authority of its own — it is a
        rendering aid, and <code>tainted</code>/<code>origin</code> remain 029 §6's actual
        enforcement.
      </>
    ),

    s4Eyebrow: "core/memory_provider.hpp — 029 §4",
    s4Heading: "The write path: extraction is an attributed effect, not a background job",
    s4Body: (
      <>
        <code>on_turn_end()</code> is where memory is written. It builds a{" "}
        <code>ChatRequest</code> from exactly this turn's messages, calls the provider's declared
        summarizer through <code>chat_stream()</code>, drains it, and — if there is real text —
        writes one <code>MemoryItem</code>. Best-effort by design: a failed stream or an empty
        response returns quietly rather than retroactively failing a turn that already succeeded.
      </>
    ),
    s4RecoLabel: "Recommended default",
    s4RecoBody: (
      <>
        Most call sites need none of <code>MemoryProvider</code>'s three template parameters to be
        anything unusual. The default in-memory worktree store types (
        <code>InMemoryWorktreeObjectStore</code>, <code>rt::InMemoryAppendLogStore</code>) are
        enough until durability across process restarts is actually required, the session's own{" "}
        <code>ChatClient</code> doubles as the summarizer with no dedicated extraction model, and{" "}
        <code>max_injected</code> is left at its default of 3. A custom store backend, a dedicated
        summarizer, or a non-default injection count are all real options — just not ones the
        common case needs.
      </>
    ),
    s4Note: (
      <>
        <strong>What is honestly not wired.</strong> <code>MemoryProvider</code> is a real{" "}
        <code>ContextProvider</code> conformer and composes with <code>HistoryProvider</code>{" "}
        through the standalone <code>assemble_context()</code> — proven, and the shape{" "}
        <code>examples/08_memory.cpp</code> follows. It is <em>not</em> occupying{" "}
        <code>AgentSession</code>'s provider slot in any shipped configuration: that slot is a
        default-constructed value member, and <code>MemoryProvider</code> has no default
        constructor, because it cannot exist without real stores and real capabilities. Widening{" "}
        <code>AgentSession</code> to take a provider pack natively is named as separate, later work
        in the header's own top comment.
      </>
    ),

    s5Eyebrow: "core/context_assembly.hpp — 005 §3",
    s5Heading: "How memory actually reaches the model",
    s5Body: (
      <>
        Memory has no private channel to the request. It is one contributor among several, and its
        output goes through the same <code>assemble_context()</code> every other provider uses:
        contributors run as independent fan-out in declared order, each with its own{" "}
        <code>ContextBudget</code>, and every message dropped for exceeding that budget becomes a
        recorded <code>ContextDrop</code>{" "}
        <code>{"{contributor index, message_id, \"over budget\"}"}</code> rather than a silent
        omission. Drops are oldest-first <em>within</em> one contributor, never a shared pool —
        so memory's budget can never be consumed by a chatty history provider.
      </>
    ),
    s5Link: "The seam itself, in full →",
    s5Note: (
      <>
        <strong>One consequence worth stating plainly.</strong> No tokenizer is wired into this
        codebase yet, so the budget arithmetic is a documented byte/4 approximation — named as an
        approximation in <code>context_assembly.hpp</code> itself, never a claim of tokenizer
        accuracy. And because memory contributes <code>role::system</code> messages, its text is
        concatenated with other system fragments by the backend translator; ADR-046 added a real{" "}
        <code>&quot;\n\n&quot;</code> separator there, because a bare concatenation could make two
        adjacent memories read as one sentence.
      </>
    ),

    s6Eyebrow: "One item, from a turn to a ranked injection",
    s6Heading: "The whole thing, end to end",
    s6Body:
      "Five stages, in the order they actually run. Every snippet below is the real shape from the header or the example, trimmed — not pseudocode.",

    s7Eyebrow: "tests/ — six files, six different claims",
    s7Heading: "The proofs",
    s7Body: (
      <>
        A load-bearing invariant without a test is not done here. Below is what each memory test
        actually establishes — including the one that exists because a real cross-tenant leak got
        through the first isolation proof.
      </>
    ),
    gateLabel: "Gate",
    s7Note: (
      <>
        <strong>Read the cross-tenant story carefully; it is the honest one.</strong> Milestone
        4's isolation proof was real and it passed — and it was blind. Both of its principals
        lived in tenant "tenant-1", so it only ever exercised same-tenant, different-id
        collisions. Two tenants that each allocate their id space independently — and therefore
        each have a user literally named "admin" — hashed to one ref name, one mount id, one
        memory worktree. Not a hypothetical: full cross-tenant memory sharing, in a release that
        had a green isolation test. The fix folds <code>tenant_id</code> into both derivations;
        the test file exists to keep the fix honest and to record that the earlier proof's
        coverage, not its logic, was the defect.
      </>
    ),

    s8Eyebrow: "The rest of 029",
    s8Heading: "What is missing, or narrower than the RFC asks for",
    s8Body: (
      <>
        Everything above is real code with tests behind it. Everything below is not — stated as
        such rather than blurred into the same paragraph. Milestone 4's own breakdown named most
        of these as deferred at the time rather than assuming them in scope, and they are still
        deferred.
      </>
    ),
    gapCols: ["What", "RFC / gate", "Where it actually stands"],
  },

  vi: {
    eyebrow: "029 — Hệ thống bộ nhớ · 005 §5",
    headingPrefix: "Bộ nhớ",
    headingHighlight: "không bao giờ rửa thẩm quyền",
    intro: (
      <>
        Bộ nhớ ở đây không phải một cơ sở dữ liệu vector được bọc lại. Nó là thứ nảy ra khi hướng
        hai thứ mà engine này vốn đã sở hữu vào một mục đích mới: kho đối tượng định địa chỉ theo
        nội dung của worktree, và ranh giới <code>ContextProvider</code>. Danh tính của một{" "}
        <code>MemoryItem</code> là digest của chính đoạn văn bản của nó, chỗ lưu của nó là một blob
        bình thường tại <code>&lt;kind&gt;/&lt;id&gt;</code> dưới một ref theo phạm vi principal,
        việc truy hồi là phép tính số học trên các trường đã lưu mà không gọi model, và nguồn gốc
        của nó là một tín hiệu tin cậy mà lớp hiển thị tôn trọng còn lớp phân quyền thì về mặt cấu
        trúc không thể bị đánh lừa.
      </>
    ),

    s1Eyebrow: "core/memory.hpp — 029 §3",
    s1Heading: "MemoryItem: có cấu trúc và có nguồn gốc, không bao giờ là một khối văn bản mờ đục",
    s1Body: (
      <>
        Bốn trường gánh toàn bộ thiết kế. <code>id</code> là digest của nội dung, nên danh tính
        chính là <em>bản chất</em> của ký ức, không phải một khóa ai đó gán cho nó.{" "}
        <code>origin</code> ghi lại ai hay cái gì đã tạo ra nó, trong lần chạy và lượt nào.{" "}
        <code>salience</code> là trọng số liên tục 0..1 mà mô hình suy giảm của 029 §7 lẽ ra sẽ
        điều chỉnh. <code>write_seq</code> là số thứ tự trong append-log của chính ref bộ nhớ —
        nguồn "độ mới" trung thực duy nhất trong một kho mà <code>Ref</code> không mang theo chuỗi
        lịch sử nào.
      </>
    ),
    sourceCols: ["memory_source", "Nhãn hiển thị (ADR-046)", "Nghĩa là gì", "Hôm nay ai ghi nó"],
    s1Note: (
      <>
        <strong>Quy tắc định hình mọi thứ bên dưới (029 §6).</strong> Không một{" "}
        <code>MemoryItem</code> nào, bất kể <code>memory_source</code> của nó là gì, được phép thỏa
        mãn một điều kiện chính sách đòi hỏi một lời khẳng định của người dùng. Đó chính là I3 áp
        dụng cho lưu trữ: bộ nhớ trở thành nội dung do model sinh ra ngay khoảnh khắc nó đi qua
        bước trích xuất, dù nó được <em>lưu</em> như trạng thái hạng nhất của engine. Engine không
        thực thi điều này bằng cách kiểm tra một trường — nó thực thi bằng cách làm cho trường đó
        không thể với tới được từ nơi ra quyết định.
      </>
    ),

    s2Eyebrow: "core/memory.hpp + core/worktree.hpp — 029 §2",
    s2Heading: "Lưu trữ: một worktree, lùi ra một cấp phạm vi — không có kho lưu trữ thứ hai",
    s2Body: (
      <>
        Worktree của một session là <code>session:s-42</code>; worktree bộ nhớ là{" "}
        <code>principal:&lt;tenant&gt;:&lt;id&gt;</code>. Cùng <code>Ref</code>, cùng{" "}
        <code>Mount</code>, cùng <code>mount_read</code>/<code>mount_write</code>, cùng phép kiểm
        tra capability — <code>memory.hpp</code> không thêm một cỗ máy worktree mới nào. Nhờ vậy bộ
        nhớ thừa hưởng miễn phí việc khử trùng lặp theo nội dung, khả năng so sánh khác biệt và
        khả năng fork, còn câu hỏi "có những ký ức nào" thì trả lời được bằng một lần liệt kê cây
        thông thường, thay vì một ngôn ngữ truy vấn mà chỉ host mới hiểu.
      </>
    ),
    s2Note: (
      <>
        <strong>
          Cả hai cái tên đều là hàm thuần túy của <code>Principal</code>, một cách có chủ ý.
        </strong>{" "}
        <code>mount_read</code>/<code>mount_write</code> so sánh những CHUỖI <code>mount_id</code>{" "}
        trần trụi mà không hề biết mỗi bên thuộc về principal nào. Một bản nháp trước đó để người
        gọi tự chọn mount id — nghĩa là một cách triển khai hoàn toàn hợp lý (bộ nhớ của mọi
        principal cùng được mount dưới một id dùng chung) sẽ khiến một capability cấp cho principal
        A thỏa mãn được phép kiểm tra đối với một <code>Mount</code> dựng cho principal B. Đó đúng
        là lớp rò rỉ "chỉ mục dùng chung" mà 029 §8/§9 nêu tên, đến từ một cái bẫy API chứ không
        phải từ một plugin thù địch.
      </>
    ),

    s3Eyebrow: "core/memory_provider.hpp — 029 §5, ADR-047",
    s3Heading: "Đường đọc: xếp hạng, gắn nhãn, tiêm vào — và đóng góp một tool",
    s3Body: (
      <>
        <code>on_context()</code> lấy thông điệp người dùng cuối cùng làm văn bản truy vấn, liệt kê
        mọi mục đã lưu, chấm điểm từng mục, sắp xếp, rồi giữ lại top <code>max_injected</code> (mặc
        định 3). Mọi đầu vào đều là dữ liệu có cấu trúc đã lưu hoặc chính văn bản của lượt này:
        không đồng hồ, không mạng, không ngẫu nhiên. Đó là điều biến việc phát lại giống nhau tới
        từng byte thành một tuyên bố kiểm chứng được, chứ không phải một nguyện vọng.
      </>
    ),
    s3Note: (
      <>
        <strong>Bề mặt giả mạo mà chính cơ chế này phải tự đóng lại (ADR-046).</strong> Nội dung
        được truy hồi là văn bản tainted, chịu ảnh hưởng của model — nó có thể chứa đúng những byte
        đánh dấu của một nhãn đáng tin hơn, dù do trùng hợp hay do một nỗ lực cố ý khiến người đọc
        tin rằng ngay sau đó là một mục có nguồn gốc khác. Mọi lần xuất hiện của token mở đầu chuỗi
        đánh dấu bên trong <code>content</code> đều bị phá vỡ bằng một khoảng trắng rộng bằng không
        trước khi nhãn thật, do cấu trúc phát ra, được gắn vào đầu — nên chuỗi đánh dấu nguyên vẹn
        chỉ có thể xuất hiện đúng ở nơi bộ hiển thị tự đặt nó. Bản thân nhãn không mang thẩm quyền
        nào — nó chỉ là một trợ giúp hiển thị, còn <code>tainted</code>/<code>origin</code> vẫn là
        cơ chế thực thi thật sự của 029 §6.
      </>
    ),

    s4Eyebrow: "core/memory_provider.hpp — 029 §4",
    s4Heading: "Đường ghi: trích xuất là một hiệu ứng có quy trách nhiệm, không phải một tác vụ nền",
    s4Body: (
      <>
        <code>on_turn_end()</code> là nơi bộ nhớ được ghi. Nó dựng một <code>ChatRequest</code> từ
        đúng các thông điệp của lượt này, gọi summarizer đã khai báo của provider qua{" "}
        <code>chat_stream()</code>, rút cạn luồng, và — nếu có văn bản thật — ghi một{" "}
        <code>MemoryItem</code>. Cố ý chỉ ở mức nỗ lực tối đa: một luồng lỗi hay một phản hồi rỗng
        sẽ lặng lẽ trả về, thay vì đánh hỏng ngược một lượt vốn đã thành công.
      </>
    ),
    s4RecoLabel: "Mặc định khuyến nghị",
    s4RecoBody: (
      <>
        Phần lớn nơi gọi không cần điều gì bất thường ở cả ba tham số template của{" "}
        <code>MemoryProvider</code>. Các kiểu store worktree trong bộ nhớ mặc định (
        <code>InMemoryWorktreeObjectStore</code>, <code>rt::InMemoryAppendLogStore</code>) là đủ
        dùng cho tới khi thực sự cần độ bền qua các lần khởi động lại tiến trình,{" "}
        <code>ChatClient</code> sẵn có của session cũng đóng luôn vai trò summarizer mà không cần
        một model trích xuất riêng, và <code>max_injected</code> được giữ ở giá trị mặc định là 3.
        Một store backend tùy chỉnh, một summarizer riêng, hay một số lượng tiêm khác mặc định đều
        là những lựa chọn có thật — chỉ là không phải thứ trường hợp phổ biến cần tới.
      </>
    ),
    s4Note: (
      <>
        <strong>Điều thành thật là chưa được nối dây.</strong> <code>MemoryProvider</code> là một
        bên tuân theo <code>ContextProvider</code> thực thụ và kết hợp được với{" "}
        <code>HistoryProvider</code> qua <code>assemble_context()</code> độc lập — đã được chứng
        minh, và đó là hình dạng mà <code>examples/08_memory.cpp</code> đi theo. Nó <em>không</em>{" "}
        chiếm giữ slot provider của <code>AgentSession</code> trong bất kỳ cấu hình nào đang xuất
        xưởng: slot đó là một thành viên giá trị được khởi tạo mặc định, còn{" "}
        <code>MemoryProvider</code> không có hàm khởi tạo mặc định, bởi nó không thể tồn tại nếu
        thiếu store thật và capability thật. Việc mở rộng <code>AgentSession</code> để nhận thẳng
        một gói provider được nêu tên là phần việc riêng, muộn hơn, ngay trong chú thích đầu file
        của header.
      </>
    ),

    s5Eyebrow: "core/context_assembly.hpp — 005 §3",
    s5Heading: "Bộ nhớ thực sự đến được với model bằng cách nào",
    s5Body: (
      <>
        Bộ nhớ không có kênh riêng nào tới request. Nó chỉ là một bên đóng góp trong số nhiều bên,
        và đầu ra của nó đi qua đúng cùng một <code>assemble_context()</code> mà mọi provider khác
        dùng: các bên đóng góp chạy fan-out độc lập theo thứ tự khai báo, mỗi bên có{" "}
        <code>ContextBudget</code> riêng, và mỗi thông điệp bị loại bỏ vì vượt ngân sách đó đều trở
        thành một <code>ContextDrop</code>{" "}
        <code>{"{chỉ số contributor, message_id, \"over budget\"}"}</code> được ghi lại, chứ không
        phải một sự bỏ sót âm thầm. Việc loại bỏ diễn ra từ cũ nhất trở đi <em>bên trong</em> một
        bên đóng góp, không bao giờ trên một quỹ chung — nên ngân sách của bộ nhớ không bao giờ có
        thể bị một history provider lắm lời tiêu hết.
      </>
    ),
    s5Link: "Toàn bộ ranh giới đó, chi tiết →",
    s5Note: (
      <>
        <strong>Một hệ quả đáng nói thẳng.</strong> Chưa có bộ đếm token (tokenizer) nào được nối
        vào codebase này, nên phép tính ngân sách là một xấp xỉ byte/4 đã được ghi rõ — được nêu
        đúng là một xấp xỉ ngay trong <code>context_assembly.hpp</code>, không bao giờ là một tuyên
        bố về độ chính xác của tokenizer. Và vì bộ nhớ đóng góp các thông điệp{" "}
        <code>role::system</code>, văn bản của nó bị bộ chuyển đổi phía backend nối liền với các
        mảnh system khác; ADR-046 đã thêm một dấu phân tách{" "}
        <code>&quot;\n\n&quot;</code> thật ở đó, vì một phép nối trần trụi có thể khiến hai ký ức
        liền kề đọc thành một câu.
      </>
    ),

    s6Eyebrow: "Một mục, từ một lượt tới một lần tiêm đã xếp hạng",
    s6Heading: "Toàn bộ câu chuyện, từ đầu tới cuối",
    s6Body:
      "Năm chặng, theo đúng thứ tự chúng thực sự chạy. Mỗi đoạn mã bên dưới là hình dạng thật lấy từ header hoặc từ ví dụ, đã lược bớt — không phải mã giả.",

    s7Eyebrow: "tests/ — sáu file, sáu tuyên bố khác nhau",
    s7Heading: "Các bằng chứng",
    s7Body: (
      <>
        Ở đây, một bất biến chịu lực mà không có kiểm thử thì chưa gọi là xong. Bên dưới là điều mà
        mỗi bài kiểm thử bộ nhớ thực sự thiết lập — kể cả bài tồn tại chỉ vì một vụ rò rỉ xuyên
        tenant có thật đã lọt qua bằng chứng cô lập đầu tiên.
      </>
    ),
    gateLabel: "Cổng",
    s7Note: (
      <>
        <strong>Hãy đọc kỹ câu chuyện xuyên tenant; đó là câu chuyện trung thực.</strong> Bằng
        chứng cô lập của Milestone 4 là thật và nó đã pass — và nó mù. Cả hai principal của nó đều
        sống trong tenant "tenant-1", nên nó chỉ từng thử trường hợp trùng trong cùng một tenant với
        id khác nhau. Hai tenant, mỗi bên cấp phát không gian id một cách độc lập — và do đó mỗi bên
        đều có một người dùng tên đúng là "admin" — băm ra cùng một tên ref, cùng một mount id, cùng
        một worktree bộ nhớ. Không phải giả định: đó là việc dùng chung bộ nhớ xuyên tenant hoàn
        toàn, trong một bản phát hành có bài kiểm thử cô lập màu xanh. Bản sửa đưa{" "}
        <code>tenant_id</code> vào cả hai phép suy diễn; file kiểm thử tồn tại để giữ cho bản sửa
        đó trung thực và để ghi lại rằng khiếm khuyết nằm ở độ phủ của bằng chứng trước đó, chứ
        không phải ở logic của nó.
      </>
    ),

    s8Eyebrow: "Phần còn lại của 029",
    s8Heading: "Những gì còn thiếu, hoặc hẹp hơn so với yêu cầu của RFC",
    s8Body: (
      <>
        Mọi thứ ở trên là mã thật với kiểm thử đứng sau. Mọi thứ bên dưới thì không — và điều đó
        được nói thẳng ra, thay vì hòa lẫn vào cùng một đoạn văn. Bản phân rã của Milestone 4 đã
        nêu tên phần lớn những mục này là bị hoãn ngay từ lúc đó chứ không mặc nhiên coi là trong
        phạm vi, và tới nay chúng vẫn còn bị hoãn.
      </>
    ),
    gapCols: ["Cái gì", "RFC / cổng", "Thực tế đang ở đâu"],
  },
} as const;

export function ApiMemoryReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];

  return (
    <section className="section" id="memory">
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

        {/* ---- 1. MemoryItem & provenance ------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="memory-item" style={{ marginTop: 48, marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/memory.hpp">{highlightCpp(memoryItemSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.sourceCols]}
              templateColumns="1.1fr 1.6fr 1.5fr 1.8fr"
              rows={memorySourceRows[lang].map((r) => [
                <code key="s">{r.source}</code>,
                <code key="l" className="api-tag">
                  {r.label}
                </code>,
                r.meaning,
                r.written,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <DocEntries ids={["entry-memory-item", "entry-memory-origin"]} />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              {t.s1Note}
            </p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. Storage ---------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="memory-storage" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/memory.hpp">{highlightCpp(memoryStorageSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <DocEntries ids={["entry-memory-ref"]} />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s2Note}</p>
          </RevealItem>

          <RevealItem>
            <Cite path="include/agentengine/core/memory.hpp" label="include/agentengine/core/memory.hpp:122 — memory_mount_id()" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Read path -------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="memory-read" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <MemReadPathDiagram />
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/memory_provider.hpp">{highlightCpp(memoryRankSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <DocEntries ids={["entry-memory-rank", "entry-memory-recall"]} />
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/memory_provider.hpp">{highlightCpp(memoryInjectionSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              {t.s3Note}
            </p>
          </RevealItem>

          <RevealItem>
            <Cite
              path="decisions/ADR-046-memory-confidence-labels-and-system-message-separator.md"
              label="decisions/ADR-046 — Judged (2026-08-14) · decisions/ADR-047 — Judged (2026-08-14)"
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Write path ------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="memory-write" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="gs-recommend">
              <span className="gs-recommend-label">{t.s4RecoLabel}</span>
              <p>{t.s4RecoBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/memory_provider.hpp">
              {highlightCpp(memoryProviderDefaultSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <MemWritePathDiagram />
          </RevealItem>

          <RevealItem>
            <DocEntries ids={["entry-memory-provider"]} />
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/08_memory.cpp">
              {highlightCpp(memoryProviderWiringSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s4Note}</p>
          </RevealItem>

          <RevealItem>
            <Cite path="examples/08_memory.cpp" label="examples/08_memory.cpp:10 — the scoping note this page repeats" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. Reaching the model ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="memory-context" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
              <a
                href="./runtime.html"
                style={{ display: "inline-block", marginTop: 12, color: "var(--accent-teal)", fontSize: "0.9rem" }}
              >
                {t.s5Link}
              </a>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 4 }}>{t.s5Note}</p>
          </RevealItem>

          <RevealItem>
            <Cite
              path="include/agentengine/core/context_assembly.hpp"
              label="include/agentengine/core/context_assembly.hpp:132 — assemble_context()"
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Worked flow ------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="memory-flow" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="glass" style={{ padding: "6px 22px", borderRadius: "var(--radius-lg)" }}>
              {memoryFlowStages[lang].map((s) => (
                <div className="flow-stage" key={s.index}>
                  <span className="flow-stage-index">{s.index}</span>
                  <div>
                    <h4>{s.title}</h4>
                    <p>{s.body}</p>
                    <CodePanel filename={`stage ${s.index}`}>{highlightCpp(s.code)}</CodePanel>
                  </div>
                </div>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <Cite
              path="include/agentengine/core/memory_provider.hpp"
              label="include/agentengine/core/memory_provider.hpp:283 — on_turn_end() · :257 — on_context()"
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 7. Proofs ------------------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="memory-proofs" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s7Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s7Heading}</h3>
              <p>{t.s7Body}</p>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="protocol-table">
          {memoryProofRows[lang].map((p) => (
            <RevealItem key={p.file}>
              <div className="protocol-row glass">
                <div>
                  <div className="protocol-name">{p.file}</div>
                  <a className="protocol-rfc" href={p.href} target="_blank" rel="noreferrer">
                    {t.gateLabel} {p.gate}
                  </a>
                </div>
                <span className="status-badge status-real">{tu.statusRealTested}</span>
                <p className="protocol-note">{p.establishes}</p>
              </div>
            </RevealItem>
          ))}
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 22, borderLeftColor: "var(--accent-pink)" }}>
              {t.s7Note}
            </p>
          </RevealItem>

          <RevealItem>
            <Cite
              path="docs/planning/milestone-4-sessions-durability-memory-breakdown.md"
              label="docs/planning/milestone-4-sessions-durability-memory-breakdown.md — Phase G/H3, the gates that govern 029 today"
            />
          </RevealItem>
        </RevealGroup>

        {/* ---- 8. Gaps -------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="memory-gaps" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s8Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s8Heading}</h3>
              <p>{t.s8Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.gapCols]}
              templateColumns="1.2fr 0.9fr 3fr"
              rows={memoryGapRows[lang].map((g) => [
                <span key="w">
                  {g.what}
                  <br />
                  <span className={`status-badge status-${g.status}`} style={{ marginTop: 6 }}>
                    {g.status === "real" ? tu.statusRealTested : tu.statusDesignedNotBuilt}
                  </span>
                </span>,
                <code key="r">{g.rfc}</code>,
                g.state,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <Cite
              path="029-Memory-System.md"
              label="029-Memory-System.md — §9 has seven gates. G1 (determinism) and G3 (no authority laundering) are directly proven; G5 (cross-principal isolation) is proven at the derivation and capability boundary, but its adversarial shared-index half needs the vector plugin that does not exist; G2, G4, G6 and G7 have no test yet."
            />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
