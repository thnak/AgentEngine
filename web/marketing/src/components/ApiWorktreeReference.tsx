import {
  gh,
  worktreeDirectoryTree,
  worktreeEntries,
  worktreeGates,
  worktreeMergeSnippet,
  worktreeMountSnippet,
  worktreeObjectModelSnippet,
  worktreeObjectStoreDedupSnippet,
  worktreeSharedMountSnippet,
  worktreeSharingModes,
  worktreeSubWorktreeSnippet,
  worktreeTurnCommitSnippet,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
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
    eyebrow: "025 — Worktree and Virtual Filesystem",
    headingPrefix: "The worktree is durable engine state,",
    headingHighlight: "not the sandbox's",
    intro: (
      <>
        A sandbox is a <strong>boundary</strong>, not a <strong>disk</strong>. If files lived
        inside it, persistence across turns, survival of a crash, portability across profiles,
        and sharing between agents would all become properties of whichever isolation technology
        happened to be selected — backwards. A <strong>worktree</strong> is a
        content-addressed object store plus a mutable tree the engine owns; a sandbox only ever{" "}
        <em>mounts</em> a view into it. File semantics are then identical under{" "}
        <code>wasm</code>, <code>native-jail</code>, and <code>remote</code> (008), and a
        destroyed sandbox loses nothing. Every piece below is real and tested; the honest gaps —
        node migration, redaction, model-assisted conflict drafting — are named in their own
        Status section, not blurred into the rest.
      </>
    ),
    treeNote: (
      <>
        <strong>As-built note:</strong> this is the target shape. Today,{" "}
        <code>/skills/&lt;name&gt;</code> materializes from <code>SkillsProvider</code>'s own
        private, per-instance store (ADR-024) — a separate content-addressed universe
        materialized side-by-side with the session's own tree, not yet one shared tree. Everything
        else in the diagram is the session's real worktree.
      </>
    ),
    modeTableColumns: ["Mode", "Semantics", "Use"],
    s1Eyebrow: "worktree.hpp §2 — the object model",
    s1Heading: (
      <>
        <code>Blob → Tree → Ref</code>: a versioned-file model, deliberately
      </>
    ),
    s1Body: (
      <>
        Snapshotting a worktree is recording one digest — cheap enough to do at every turn
        boundary. Diffing two states is a tree comparison, so "what did this agent change?" is
        answerable. Deduplication is free: the same file written by two agents is stored once,
        proven directly by watching the store's own object counts not grow on a duplicate write,
        not merely inferred from digest equality. Identity is the digest, so artifacts (010 §4),
        blobs in messages (003 §3), and worktree files are the same objects with the same
        provenance.
      </>
    ),
    s1DedupNote: (
      <>
        <strong><code>put_blob</code>/<code>put_tree</code> take no digest parameter at all</strong>{" "}
        — the store computes it from the bytes (or from a Tree's canonical serialization) every
        time, so nothing upstream can ever claim content hashes to a digest it doesn't actually
        produce. Dedup is checked the same honest way: the same content put twice yields the same
        digest AND <code>blob_count()</code> stays at 1, not assumed from digest equality alone.
      </>
    ),
    s2Eyebrow: "worktree.hpp §3 — sub-worktrees",
    s2Heading: "Every agent gets its own branch of the same disk",
    s2Body: (
      <>
        A session may run several agents (001 §4); each sub-worktree is created with a declared
        sharing mode. The default is chosen by <strong>concurrency, not taste</strong>: agents
        running sequentially default to <code>shared</code> — the natural reading of "they work
        on the same disk" — while concurrent agents default to <code>branch</code>, because
        concurrent blind writes to one tree is precisely how multi-agent systems destroy each
        other's work. That default lives one layer up, in whatever scheduler calls{" "}
        <code>create_sub_worktree</code> — this header always takes <code>mode</code> as an
        explicit argument and infers nothing.
      </>
    ),
    s2SharedNote: (
      <>
        <strong>Two sandboxes, one worktree — proven, not asserted.</strong> Minting two{" "}
        <code>shared</code> executors gives them DISTINCT mount ids but the SAME backing ref: a
        write through one mount is visible through the other's immediately, because both resolve
        through the identical <code>Ref</code>. The mount is the sandbox-facing view; the ref is
        the durable thing underneath it — exactly the "independent of whichever sandbox happens
        to be attached" property this page opens with.
      </>
    ),
    s3Eyebrow: "worktree.hpp §4 — concurrency and merge",
    s3Heading: "Conflicts are surfaced, never guessed at",
    s3Body: (
      <>
        A <code>branch</code> sub-worktree merges back on join: three-way, against the common
        ancestor, per name at each tree level — disjoint changes merge automatically, identical
        content merges trivially, and a genuine fork is a <code>MergeConflict</code>, both
        versions retained, never resolved by guessing or by last-writer-wins. A model may draft a
        proposed merge from the two versions, but that draft is <code>Tainted</code> data like any
        other model output (I3) — it is presented for the same argument-hash-bound approval every
        other irreversible effect requires, never auto-applied because "the model resolved it."
      </>
    ),
    s3ResidualNote: (
      <>
        <strong>One writer per tree, best-effort today — named, not silently closed.</strong>{" "}
        Writes serialize through the caller's own coordination; <code>merge_branch_into_parent</code>{" "}
        re-reads the parent immediately before committing and fails closed if it moved since, which
        narrows the race but does not eliminate it — full elimination needs a compare-and-set
        primitive on the store that does not exist yet. The concurrency proof below runs at a
        reduced, machine-safe scale for exactly this reason: real OS threads racing the reference
        store's plain hash map would themselves be undefined behavior, unrelated to what the test is
        trying to prove.
      </>
    ),
    s4Eyebrow: "worktree.hpp §5 — mounts and capabilities",
    s4Heading: "Two layers of defense, not one",
    s4Body: (
      <>
        A worktree subtree becomes visible to a sandbox only through a capability —{" "}
        <code>cap::FsRead</code>/<code>cap::FsWrite</code>, mount-id and path-prefix checked
        before any store access at all, reusing the exact same subsumption primitive{" "}
        <code>CapabilitySet::contains</code> already uses elsewhere, not a second path-matching
        routine invented here. That's layer one, over the content-addressed tree. Layer two sits
        below it, at the real filesystem: on Windows, every guest-relative path is turned into an
        already-verified-safe <code>HANDLE</code> before the OS ever opens it, rejecting a full
        corpus of escape techniques — <code>..</code>, absolute redirects, symlinks/junctions
        crossing the mount boundary, alternate data streams, <code>\\?\</code> prefixes, unicode
        tricks, and TOCTOU re-resolution — each proven with a positive control alongside the
        negative one. Linux isn't built yet (021 §2's platform sequencing).
      </>
    ),
    s5Eyebrow: "worktree.hpp §6 — persistence and lifecycle",
    s5Heading: "Rewind is assignment, never a history edit",
    s5Body: (
      <>
        The current tree is committed at every turn boundary, and its digest recorded against the
        turn — per-turn rewind at no meaningful extra cost. Restoring a turn re-commits its digest
        as the new current head rather than editing history, so a second rewind can always recover
        the state that existed just before the first one. Restore-after-restart is the identical
        digest-fetch mechanism. Retention/GC and node migration are the two pieces still fully
        design-only — see Status below.
      </>
    ),
    s6Eyebrow: "worktree.hpp §7 — what the agent sees",
    s6Heading: "Ordinary files, on purpose",
    s6Body: (
      <>
        <code>open()</code>, <code>pathlib</code>, <code>os.listdir</code>, <code>ls</code>,{" "}
        <code>cat</code> — all work, because a mount is a real filesystem view, not an API a
        guest calls. The agent is never told it's a virtual disk. Two host-side behaviours make
        this pay off, both proven against real files through the same host-directory sync bridge:
        files written under <code>/out</code> are collected into the conversation as artifacts,
        and inputs are mounted rather than pasted into the prompt — a 40&nbsp;MB CSV costs a
        mount, not a context window.
      </>
    ),
    s7Eyebrow: "worktree_scoping.hpp — ADR-032",
    s7Heading: "The same mechanism, one layer up: workflow executors",
    s7Body: (
      <>
        A workflow's <code>FunctionExecutor</code> gets its own <code>ExecutorWorktreeGrant</code>{" "}
        — which sharing mode, which mount, which capabilities — minted or resumed explicitly,
        never assumed. The minting/resumption logic itself is real and tested; what isn't real yet
        is the wiring from a running <code>WorkflowSupervisor</code> into that grant, since no
        production host builds a <code>FunctionExecutor</code> fleet today.
      </>
    ),
    statusEyebrow: "025 §9 — promotion gates, graded honestly",
    statusHeading: "Real where it's tested, named where it isn't",
    statusBody:
      "Every gate below is graded against actual test evidence, not the RFC's own aspirational wording — the same discipline this whole page follows.",
    gateTableColumns: ["Gate", "Claim", "Verdict"],
  },
  vi: {
    eyebrow: "025 — Worktree and Virtual Filesystem",
    headingPrefix: "Worktree là trạng thái bền vững của engine,",
    headingHighlight: "không phải của sandbox",
    intro: (
      <>
        Một sandbox là một <strong>ranh giới</strong>, không phải một <strong>ổ đĩa</strong>.
        Nếu file sống bên trong nó, tính bền vững qua các lượt, khả năng sống sót qua crash, tính
        di động giữa các profile, và khả năng chia sẻ giữa các agent đều sẽ trở thành thuộc tính
        của bất kỳ công nghệ cách ly nào được chọn — ngược đời. Một <strong>worktree</strong> là
        một object store theo nội dung cộng một cây mutable mà engine sở hữu; một sandbox chỉ bao
        giờ <em>mount</em> một view vào đó. Ngữ nghĩa file khi đó giống hệt dưới <code>wasm</code>,{" "}
        <code>native-jail</code>, và <code>remote</code> (008), và một sandbox bị hủy không mất gì
        cả. Mọi phần bên dưới đều có thật và đã kiểm thử; các khoảng trống trung thực — node
        migration, xóa dữ liệu (redaction), soạn thảo xung đột có sự hỗ trợ của model — được nêu
        riêng trong phần Trạng thái, không bị trộn lẫn vào phần còn lại.
      </>
    ),
    treeNote: (
      <>
        <strong>Ghi chú as-built:</strong> đây là hình dạng mục tiêu. Hôm nay,{" "}
        <code>/skills/&lt;name&gt;</code> được vật chất hóa từ store riêng, theo từng instance của{" "}
        <code>SkillsProvider</code> (ADR-024) — một vũ trụ theo nội dung riêng biệt, vật chất hóa
        song song với cây riêng của session, chưa phải một cây chung. Phần còn lại trong sơ đồ là
        worktree thật của session.
      </>
    ),
    modeTableColumns: ["Chế độ", "Ngữ nghĩa", "Dùng cho"],
    s1Eyebrow: "worktree.hpp §2 — mô hình đối tượng",
    s1Heading: (
      <>
        <code>Blob → Tree → Ref</code>: một mô hình kiểu versioned-file, một cách cố ý
      </>
    ),
    s1Body: (
      <>
        Chụp nhanh (snapshot) một worktree là ghi lại một digest — đủ rẻ để làm ở mỗi ranh giới
        turn. Diff hai trạng thái là một phép so sánh cây, nên "agent này đã thay đổi gì?" có thể
        trả lời được. Loại bỏ trùng lặp (dedup) là miễn phí: cùng một file được hai agent ghi chỉ
        được lưu một lần, được chứng minh trực tiếp bằng cách theo dõi số lượng object của store
        không tăng khi ghi trùng, không chỉ suy ra từ việc digest bằng nhau. Định danh chính là
        digest, nên artifact (010 §4), blob trong message (003 §3), và file worktree đều là cùng
        một loại object với cùng nguồn gốc.
      </>
    ),
    s1DedupNote: (
      <>
        <strong><code>put_blob</code>/<code>put_tree</code> không nhận tham số digest nào cả</strong>{" "}
        — store tự tính nó từ các byte (hoặc từ chuỗi hóa chuẩn tắc của một Tree) mỗi lần, nên
        không có gì ở tầng trên có thể khai báo nội dung băm ra một digest mà nó thực sự không tạo
        ra. Dedup được kiểm tra theo đúng cách trung thực đó: cùng nội dung put hai lần cho ra
        cùng digest VÀ <code>blob_count()</code> vẫn giữ ở 1, không giả định chỉ từ việc digest
        bằng nhau.
      </>
    ),
    s2Eyebrow: "worktree.hpp §3 — sub-worktree",
    s2Heading: "Mỗi agent nhận một nhánh riêng của cùng một ổ đĩa",
    s2Body: (
      <>
        Một session có thể chạy nhiều agent (001 §4); mỗi sub-worktree được tạo với một chế độ
        chia sẻ khai báo rõ. Mặc định được chọn theo <strong>tính đồng thời, không phải theo sở
        thích</strong>: các agent chạy tuần tự mặc định là <code>shared</code> — cách đọc tự nhiên
        của "chúng làm việc trên cùng một ổ đĩa" — còn các agent chạy đồng thời mặc định là{" "}
        <code>branch</code>, vì ghi mù đồng thời lên một cây chính là cách các hệ multi-agent phá
        hỏng công việc của nhau. Mặc định đó nằm ở một tầng cao hơn, ở bất kỳ scheduler nào gọi{" "}
        <code>create_sub_worktree</code> — header này luôn nhận <code>mode</code> như một tham số
        tường minh và không suy luận gì cả.
      </>
    ),
    s2SharedNote: (
      <>
        <strong>Hai sandbox, một worktree — được chứng minh, không chỉ khẳng định.</strong> Tạo
        mới hai executor <code>shared</code> cho chúng mount id KHÁC NHAU nhưng CÙNG một ref nền
        — một lần ghi qua mount này được thấy ngay qua mount kia, vì cả hai đều phân giải tới đúng
        một <code>Ref</code>. Mount là view hướng-sandbox; ref là thứ bền vững nằm bên dưới nó —
        đúng thuộc tính "độc lập với bất kỳ sandbox nào đang được gắn" mà trang này mở đầu.
      </>
    ),
    s3Eyebrow: "worktree.hpp §4 — tương tranh và merge",
    s3Heading: "Xung đột được phơi bày, không bao giờ bị đoán",
    s3Body: (
      <>
        Một sub-worktree <code>branch</code> merge ngược lại khi join: ba-chiều, so với ancestor
        chung, theo từng tên ở mỗi tầng cây — thay đổi rời rạc tự động merge, nội dung giống hệt
        merge tầm thường, và một fork thật sự là một <code>MergeConflict</code>, cả hai phiên bản
        được giữ lại, không bao giờ được giải quyết bằng cách đoán hay theo kiểu người-ghi-cuối-
        thắng. Một model có thể soạn một đề xuất merge từ hai phiên bản, nhưng bản soạn đó là dữ
        liệu <code>Tainted</code> giống như bất kỳ đầu ra nào khác của model (I3) — nó được trình
        bày để chờ đúng loại phê duyệt gắn với argument-hash mà mọi effect không thể đảo ngược
        khác đòi hỏi, không bao giờ tự áp dụng chỉ vì "model đã giải quyết nó."
      </>
    ),
    s3ResidualNote: (
      <>
        <strong>Một writer cho mỗi cây, best-effort hôm nay — được nêu tên, không âm thầm coi là
        đã đóng.</strong> Ghi được serialize thông qua sự phối hợp riêng của caller;{" "}
        <code>merge_branch_into_parent</code> đọc lại parent ngay trước khi commit và fail closed
        nếu nó đã di chuyển — điều này thu hẹp race nhưng không loại bỏ nó: loại bỏ hoàn toàn cần
        một nguyên thủy compare-and-set trên store mà hiện chưa tồn tại. Vì đúng lý do này, phép
        chứng minh tương tranh bên dưới chạy ở quy mô giảm, an toàn cho máy: luồng OS thật đua nhau
        trên hash map thuần của store tham chiếu tự nó đã là hành vi không xác định, không liên
        quan tới điều test đang cố chứng minh.
      </>
    ),
    s4Eyebrow: "worktree.hpp §5 — mount và capability",
    s4Heading: "Hai lớp phòng thủ, không phải một",
    s4Body: (
      <>
        Một subtree của worktree chỉ hiển thị với một sandbox thông qua một capability —{" "}
        <code>cap::FsRead</code>/<code>cap::FsWrite</code>, mount-id và path-prefix được kiểm tra
        trước khi có bất kỳ truy cập store nào, tái sử dụng đúng nguyên thủy subsumption mà{" "}
        <code>CapabilitySet::contains</code> đã dùng ở nơi khác, không phải một routine khớp
        đường dẫn thứ hai được phát minh riêng ở đây. Đó là lớp một, trên cây theo nội dung. Lớp
        hai nằm bên dưới nó, ở filesystem thật: trên Windows, mọi đường dẫn tương đối từ guest
        được biến thành một <code>HANDLE</code> đã được xác minh an toàn trước khi OS mở nó, từ
        chối một corpus đầy đủ các kỹ thuật thoát ly — <code>..</code>, chuyển hướng tuyệt đối,
        symlink/junction vượt ranh giới mount, alternate data stream, tiền tố <code>\\?\</code>,
        các thủ thuật unicode, và TOCTOU re-resolution — mỗi cái được chứng minh cùng một positive
        control bên cạnh negative control. Linux chưa được xây (thứ tự ưu tiên nền tảng của 021 §2).
      </>
    ),
    s5Eyebrow: "worktree.hpp §6 — bền vững và vòng đời",
    s5Heading: "Rewind là gán (assignment), không bao giờ là sửa lịch sử",
    s5Body: (
      <>
        Cây hiện tại được commit ở mỗi ranh giới turn, và digest của nó được ghi lại cùng turn —
        rewind theo từng turn với chi phí thêm không đáng kể. Khôi phục một turn commit lại digest
        của nó như head hiện tại mới thay vì sửa lịch sử, nên lần rewind thứ hai luôn có thể khôi
        phục trạng thái tồn tại ngay trước lần rewind đầu. Restore sau khi restart là đúng cơ chế
        digest-fetch đó. Retention/GC và node migration là hai phần vẫn còn hoàn toàn ở dạng thiết
        kế — xem phần Trạng thái bên dưới.
      </>
    ),
    s6Eyebrow: "worktree.hpp §7 — agent nhìn thấy gì",
    s6Heading: "File bình thường, một cách cố ý",
    s6Body: (
      <>
        <code>open()</code>, <code>pathlib</code>, <code>os.listdir</code>, <code>ls</code>,{" "}
        <code>cat</code> — tất cả đều chạy được, vì một mount là một filesystem view thật, không
        phải một API mà guest gọi. Agent không bao giờ được cho biết đó là một ổ đĩa ảo. Hai hành
        vi phía host làm điều này đáng giá, cả hai đều được chứng minh trên file thật qua cùng một
        cầu nối đồng bộ thư mục host: file được ghi ở <code>/out</code> được thu thập vào cuộc hội
        thoại như artifact, và input được mount thay vì dán vào prompt — một file CSV 40&nbsp;MB
        chỉ tốn một mount, không tốn context window.
      </>
    ),
    s7Eyebrow: "worktree_scoping.hpp — ADR-032",
    s7Heading: "Đúng cơ chế đó, ở một tầng cao hơn: executor của workflow",
    s7Body: (
      <>
        Một <code>FunctionExecutor</code> của workflow nhận <code>ExecutorWorktreeGrant</code>{" "}
        riêng của nó — chế độ chia sẻ nào, mount nào, capability nào — được tạo mới hoặc khôi phục
        một cách tường minh, không bao giờ mặc định. Bản thân logic tạo mới/khôi phục có thật và
        đã kiểm thử; điều chưa có thật là việc đấu nối từ một <code>WorkflowSupervisor</code> đang
        chạy vào cấp phát đó, vì chưa có host production nào xây một fleet{" "}
        <code>FunctionExecutor</code> hôm nay.
      </>
    ),
    statusEyebrow: "025 §9 — các gate promotion, chấm điểm trung thực",
    statusHeading: "Có thật ở nơi đã kiểm thử, được nêu tên ở nơi chưa",
    statusBody:
      "Mỗi gate bên dưới được chấm điểm dựa trên bằng chứng test thật, không phải theo lời văn kỳ vọng của RFC — đúng kỷ luật mà toàn bộ trang này tuân theo.",
    gateTableColumns: ["Gate", "Yêu cầu", "Kết luận"],
  },
} as const;

export function ApiWorktreeReference() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="worktree">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        <RevealGroup className="spec-layout">
          <RevealGroup>
            <RevealItem>
              <CodePanel filename="session:s-42/">{worktreeDirectoryTree}</CodePanel>
            </RevealItem>
            <RevealItem>
              <p className="gs-note" style={{ marginTop: 20 }}>{t.treeNote}</p>
            </RevealItem>
          </RevealGroup>

          <RevealGroup>
            <RevealItem>
              <ApiTable
                columns={[...t.modeTableColumns]}
                templateColumns="1.2fr 2.4fr 1.4fr"
                rows={worktreeSharingModes[lang].map((m) => [
                  <code key="mode">{m.mode}</code>,
                  m.semantics,
                  m.use,
                ])}
              />
            </RevealItem>
          </RevealGroup>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="object-model">
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="worktree.hpp">{highlightCpp(worktreeObjectModelSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <div className="doc-entries">
              {worktreeEntries[lang]
                .filter((e) => e.id === "object-store")
                .map((e) => (
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
            <CodePanel filename="tests/test_worktree_object_store.cpp">
              {highlightCpp(worktreeObjectStoreDedupSnippet)}
            </CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s1DedupNote}</p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="sub-worktrees">
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="worktree.hpp">{highlightCpp(worktreeSubWorktreeSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="tests/test_workflow_worktree_scoping.cpp">
              {highlightCpp(worktreeSharedMountSnippet)}
            </CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s2SharedNote}</p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="concurrency-merge">
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="worktree.hpp">{highlightCpp(worktreeMergeSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s3ResidualNote}</p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="mounts-capabilities">
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="worktree.hpp">{highlightCpp(worktreeMountSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <div className="doc-entries">
              {worktreeEntries[lang]
                .filter((e) => e.id === "path-escape" || e.id === "mount-sync")
                .map((e) => (
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
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="persistence-lifecycle">
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="worktree.hpp">{highlightCpp(worktreeTurnCommitSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="agent-view">
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="workflow-integration">
              <span className="eyebrow">{t.s7Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s7Heading}</h3>
              <p>{t.s7Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="doc-entries">
              {worktreeEntries[lang]
                .filter((e) => e.id === "workflow-grant")
                .map((e) => (
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
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="worktree-status">
              <span className="eyebrow">{t.statusEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.statusHeading}</h3>
              <p>{t.statusBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.gateTableColumns]}
              templateColumns="1.2fr 2fr 2.6fr"
              rows={worktreeGates[lang].map((g) => [<code key="gate">{g.gate}</code>, g.claim, g.verdict])}
            />
          </RevealItem>
          <RevealItem>
            <div style={{ marginTop: 16, display: "flex", gap: 16, flexWrap: "wrap" }}>
              <a href={gh("025-Worktree-and-Virtual-Filesystem.md")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                025-Worktree-and-Virtual-Filesystem.md
              </a>
              <a href={gh("include/agentengine/core/worktree.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                include/agentengine/core/worktree.hpp
              </a>
              <a href={gh("decisions/ADR-014-worktree-mount-path-canonicalization.md")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                decisions/ADR-014
              </a>
              <a href={gh("decisions/ADR-032-workflow-executor-worktree-scoping.md")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                decisions/ADR-032
              </a>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
