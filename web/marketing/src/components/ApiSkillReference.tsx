import {
  gh,
  genericSkills,
  inlineSkillSourceEagerParseSnippet,
  inlineSkillSourceExampleSnippet,
  inlineSkillSourceFailureConstructorSnippet,
  inlineSkillSourceShapeSnippet,
  minimalSkillSnippet,
  skillCollisionSnippet,
  skillFrontmatterFields,
  skillOnDemandMountSnippet,
  skillSourceConceptSnippet,
  skillSourceEntries,
  skillToolScopingSnippet,
  skillTwoSourcesMountSnippet,
  skillsProviderApiSnippet,
} from "../data/apiContent";
import { SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
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

const directoryTree = `skill-name/
├── SKILL.md          # YAML frontmatter + Markdown instructions
│                      # (the frontmatter IS the manifest)
├── scripts/           # optional executable code
├── references/        # optional documentation
└── assets/            # optional templates and resources`;

const copy = {
  en: {
    eyebrow: "009 §8 — Plugin and Extension System",
    headingPrefix: "Skills: filesystem-mounted competence,",
    headingHighlight: "not a tool call",
    intro: (
      <>
        A <strong>skill</strong> bundles instructions, resources, and optional scripts that
        give an agent a competence on demand — a different thing from a <code>Tool</code>: no
        typed args, no <code>invoke()</code>, no capability-gated call. AgentEngine adopts{" "}
        <code>SKILL.md</code> (the open <code>agentskills.io</code> standard, Apache-2.0) as
        the format of record — the identical layout Anthropic's own products and Microsoft
        Agent Framework already use. The format, sourcing, and mounting below are real and
        tested; the one piece still design-only is called out in its own section further down.
      </>
    ),
    recoLabel: "Recommended default",
    recoBody: (
      <>
        Only two frontmatter fields are required — <code>name</code> and{" "}
        <code>description</code>; <code>parse_skill_md</code> rejects a document missing either
        one with its own error code. <code>license</code>, <code>compatibility</code>,{" "}
        <code>metadata</code>, and <code>allowed-tools</code> are all optional and can be left
        out entirely. A minimal skill is just these two fields plus a Markdown body:
      </>
    ),
    treeNote: (
      <>
        <strong>Skill names are labels, not identifiers.</strong> Skills are namespaced per
        origin so one fetched from a remote source can never shadow a local one.
      </>
    ),
    tableColumns: ["Frontmatter field", "Type", "Required", "Notes"],
    s1Eyebrow: "core/skill_source.hpp — where a skill comes from",
    s1Heading: (
      <>
        Two real <code>SkillSource</code> implementations, one concept
      </>
    ),
    s1Body: (
      <>
        Type-erased at runtime, not a compile-time template pack. A session declares which
        sources to load skills from as ordinary runtime configuration — disk paths, inline
        bundles — the same shape <code>ToolTable</code> already accepts for tools. Both real
        sources below satisfy the same <code>SkillSource</code> concept and get wrapped
        identically by <code>make_skill_source_descriptor</code> into the type-erased{" "}
        <code>SkillSourceDescriptor</code> that a <code>SkillsProvider</code> holds a list of.
        A third source, a remote registry say, would need only those same two methods.
      </>
    ),
    s1Note: (
      <>006 §6 — <code>ToolTable</code> accepts this identical runtime-configuration shape for tools.</>
    ),
    s2Eyebrow: "InlineSkillSource, in full",
    s2Heading: "The whole class is eleven lines — here's every one of them",
    s2Body: (
      <>
        <code>InlineSkillSource</code> is deliberately the simplest possible{" "}
        <code>SkillSource</code> conformer: two constructors that store what they're given,
        and two accessors that hand it back. No parsing, no I/O, no cache to go stale — the
        caller does the work, usually via <code>parse_skill_md</code> against a string
        literal, before this type ever sees it. The second constructor takes a{" "}
        <code>result&lt;std::vector&lt;SkillSourceResult&gt;&gt;</code> directly, so a caller
        whose own construction-time parsing can itself fail can hand this class a failure and
        get that exact failure back, unchanged, from <code>load_skills()</code>.{" "}
        <code>builtin_skills.hpp</code>'s five generic skills and the PDF-tool catalog's{" "}
        <code>extracting-document-text</code> skill both take this path.
      </>
    ),
    s2ExampleIntro: (
      <>
        A worked example — build one skill entirely in memory and hand it to a{" "}
        <code>SkillsProvider</code> the same way a disk-backed skill would be, no filesystem
        involved at any point:
      </>
    ),
    s2FailureIntro: (
      <>
        The second constructor's whole point, isolated: hand it a failure instead of a real
        skill list, and get that <em>exact</em> failure back out — not a generic "load failed",
        not an empty list to silently work around. Whatever error a caller's own eager parsing
        produced is what a later <code>load_skills()</code> call sees, unchanged, every time —
        and it still holds after <code>make_skill_source_descriptor</code> erases the concrete
        type:
      </>
    ),
    s2EagerParseIntro: (
      <>
        And the real caller this exists for — the PDF-tool catalog's own{" "}
        <code>extracting-document-text</code> skill source, verbatim. Parsing happens once,
        inside the immediately-invoked lambda, before <code>InlineSkillSource</code> is ever
        constructed; the class itself never parses anything, it only ever replays whichever{" "}
        <code>result</code> it was handed:
      </>
    ),
    s2Note: (
      <>
        <strong>Why this exists, not just how:</strong> the five built-in skills this engine
        ships — <code>using-the-code-interpreter</code>, <code>using-codeact</code>, and
        others — are compiled into the binary as raw string literals.{" "}
        <code>builtin_skills.hpp</code> parses each one the same way, at construction time, so
        a deployment can never omit or move a loose <code>SKILL.md</code> file that a core
        skill depends on. It constructs an <code>InlineSkillSource</code>, the same path the
        PDF-tool catalog's own <code>extracting-document-text</code> skill takes, rather than
        hand-assembling the type-erased <code>SkillSourceDescriptor</code> itself: every
        compiled-in skill source this engine ships goes through the one class built for
        exactly this. Composing several <code>InlineSkillSource</code>-backed descriptors side
        by side — two independent origins in one <code>SkillsProvider</code>, plus the
        on-demand mount lifecycle that unlocks their tools — is covered further down, with its
        own snippets, in <a href="#skill-loading">how a skill loads</a>.
      </>
    ),
    s3Eyebrow: "How a skill loads — §8b",
    s3Heading: "Three tiers of disclosure, all of them just filesystem reads",
    step1Title: "Always advertised",
    step1Body: (
      <>
        Every mounted skill's <code>name</code> + <code>description</code> — roughly
        100 tokens — is visible up front, for every skill, regardless of catalog size.
      </>
    ),
    step2Title: "Loaded on activation",
    step2Body: (
      <>
        The <code>SKILL.md</code> body (target &lt;5,000 tokens) loads only once the
        agent decides this skill applies.
      </>
    ),
    step3Title: "Loaded on demand",
    step3Body: (
      <>
        Bundled <code>scripts/</code>, <code>references/</code>, and <code>assets/</code>{" "}
        are read only when the instructions in step 02 tell the agent to. A bundled
        script's stdout enters the context — its source does not.
      </>
    ),
    s3Note: (
      <>
        A skill is mounted read-only at <code>/skills/&lt;name&gt;</code> and the agent reads
        it with ordinary file operations — <strong>no <code>load_skill</code> /{" "}
        <code>read_skill_resource</code> tool wrappers exist; the mount is the mechanism.</strong>{" "}
        This is a deliberate divergence from Microsoft Agent Framework, which uses exactly
        those two tools (plus <code>run_skill_script</code>) as a fixed three-tool surface
        regardless of catalog size. Loading is dynamic but snapshotted per run: a skill
        loaded mid-run does not retroactively change what earlier turns were permitted to do.
      </>
    ),
    s4Eyebrow: "core/skill_provider.hpp — mounting",
    s4Heading: (
      <>
        <code>SkillsProvider</code> — a real <code>ContextProvider</code>, not a stub
      </>
    ),
    s4Body: (
      <>
        Resolves every declared source, mounts each resolved skill read-only through{" "}
        <code>worktree.hpp</code>'s real Tree/Ref/Mount machinery, and contributes one{" "}
        <code>role::system</code> advertisement message naming every mounted skill. A
        mounted skill's <code>Mount.mount_id</code> is the bare name, such as{" "}
        <code>"using-codeact"</code>, not the literal <code>"/skills/&lt;name&gt;"</code>{" "}
        string. <code>mount_id</code> is purely the capability-matching key that{" "}
        <code>cap::FsRead</code> and <code>cap::FsWrite</code> compare against, which gives an
        operator genuine per-skill granularity: read access can be granted to one skill's
        files without exposing every other mounted skill. The logical{" "}
        <code>/skills/&lt;name&gt;</code> path is unaffected by this choice; it only ever
        reaches the model through the advertisement message.
      </>
    ),
    flowInlineSub: "bundled SKILL.md text",
    flowDiskSub: "a real host directory",
    flowResolveArrow: "resolve_and_mount() — any collision fails the whole call closed",
    flowWorktreeSub: "each resolved skill mounted read-only at /skills/<name>",
    flowOnContext: "on_context()",
    flowAdMessageSub: "names every mounted skill — ~100 tokens, regardless of catalog size",
    s4Note: (
      <>
        <strong>Anti-shadowing is a refusal, never a last-source-wins.</strong> Built entirely
        into local vectors and assigned to <code>mounted()</code>'s backing state only on total
        success, a collision anywhere in the loop fails the whole <code>on_context()</code>{" "}
        call closed — leaving zero skills mounted, not just the ones that happened to process
        first.
      </>
    ),
    s4TwoSourcesIntro: (
      <>
        For contrast, the success case: two differently named skills from two different
        sources mount together with no conflict at all.
      </>
    ),
    s5Eyebrow: "core/skill_tool_scoping.hpp — §8c enforcement",
    s5Heading: (
      <>
        <code>allowed-tools</code> becomes a real restriction — if both sides stay in sync
      </>
    ),
    s5Body: (
      <>
        <code>scope_tools_to_mounted_skills()</code> filters a <code>ToolTable</code> down to
        the names a caller allows, typically <code>SkillsProvider::allowed_tool_names()</code>{" "}
        unioned with an always-on base set. Real enforcement requires applying that filter on
        both sides of the boundary: what's declared to the model, and what{" "}
        <code>invoke_tool()</code> actually authorizes at call time. Filtering only the
        declared side and leaving a broader table at the invocation site is cosmetic — a tool
        "hidden" from the model's list is still callable if the invocation-time table still
        contains it. The invoke-time check has to be the real one.
      </>
    ),
    s5Note: (
      <>
        <code>ContextContribution.tools</code>, computed each turn's <code>on_context</code>,
        is the declared side; <code>invoke_tool()</code> is the invocation side. I3 — model
        output is data, never itself an authorization decision.
      </>
    ),
    s6Eyebrow: "Phase 3 addendum — ADR-024",
    s6Heading: '"Resolved" and "mounted" are independent concerns',
    s6Body: (
      <>
        Every configured skill is <strong>resolved</strong> unconditionally at session start:
        its files are materialized and its <code>cap::FsRead</code> is already granted,
        regardless of what happens next. <code>MountedSkillsState</code> tracks a narrower,
        agent-triggered subset — which resolved skills are currently <strong>mounted</strong>,
        meaning declared to the model and re-injected into context. Mounting a skill grants no
        new authority; it only activates visibility into capability that was already
        unconditionally provisioned. It's a deliberately plain mutable set, not a token:{" "}
        <code>mount()</code> is idempotent, so an agent unsure of a skill's state can call it
        again for free.
      </>
    ),
    s6Note: (
      <>
        I3 — model output is data, never itself an authorization decision. Resolution grants{" "}
        <code>cap::FsRead</code> once at session start; mounting only toggles what's declared
        to the model.
      </>
    ),
    s6ExampleIntro: (
      <>
        The real pipeline, not a description of it: <code>examples/11_skill_mount.cpp</code> calls
        the same <code>word_count</code> tool call twice against the same{" "}
        <code>invoke_tool()</code> pipeline. It's rejected while <code>word-counter</code> is
        unmounted, accepted once it's mounted — nothing else in the call changes:
      </>
    ),
    s6CliNote: (
      <>
        <code>tools/cli_chat.cpp</code> is the live, interactive counterpart: a real model calls{" "}
        <code>mount_skill</code> mid-conversation — the same tool this section describes, rendered
        through the ordinary <code>tool_call_started</code>/<code>tool_call_finished</code> events
        (
        <a href={`${SITE_BASE}/api/events.html`}>Events page</a>) as{" "}
        <code>"-&gt; calling tool 'mount_skill'"</code> / <code>"&lt;- tool call OK"</code>, with a
        startup banner distinguishing resolved, materialized, and currently-mounted skills before
        any of that happens. Needs a real model credential — see the file's own build guard.
      </>
    ),
    s7Eyebrow: "core/composed_context_provider.hpp — wiring into AgentSession",
    s7Heading: (
      <>
        One <code>HistoryProviderT</code> slot, two contributors — and a real ordering bug
        the tests now pin down
      </>
    ),
    s7Body: (
      <>
        <code>AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;</code> has exactly one
        history-provider slot. <code>ComposedContextProvider&lt;H, S&gt;</code> composes a
        history provider and a <code>SkillsProvider</code> into that one slot via{" "}
        <code>assemble_context</code>, engaged once via its constructor (or an explicit{" "}
        <code>engage()</code> call) — never rebuilt inside <code>on_context()</code>, or{" "}
        <code>SkillsProvider</code>'s resolve-once/freeze guarantee would silently break every
        turn.
      </>
    ),
    wrongLabel: "Wrong — history pushed first",
    wrong: "Wire order: [...whole conversation..., skills advertisement]. The system-message advertisement lands after every prior turn, the opposite of ordinary system-prompt placement. A live wire dump confirmed this ordering degrades the model's adherence to the advertisement.",
    rightLabel: "Right — skills pushed first",
    right: (
      <>
        Wire order: [skills advertisement, ...whole conversation...]. Contributor order is
        wire order, <code>assemble_context</code>'s own rule, so declaring skills before
        history in the constructor is the whole fix.
      </>
    ),
    s7Note: (
      <>
        <strong>Order matters, and the first version had it backwards.</strong> Contributors
        concatenate in declared order. Pushing history first put the skills' system-message
        advertisement after the entire conversation on every real request, the opposite of
        ordinary system-prompt placement and exactly the kind of thing that degrades a model's
        adherence to it. A live wire dump confirmed the effect; it wasn't a hypothetical.
        Skills now push first, and{" "}
        <code>test_agent_session_skills_real_backend.cpp</code> asserts message order, not
        just presence, so this can't silently regress.
      </>
    ),
    s7Trailing: (
      <>
        <code>ComposedContextProvider&lt;Ms...&gt;</code> is the one general composite type for
        any N real <code>ContextProvider</code>s (history, skills, memory, …) in one slot, same{" "}
        <code>assemble_context</code> underneath — declare <code>Ms...</code> in the wire order
        you want, no separate hand-written composite needed. Walked through in more depth on the{" "}
        <a href={`${SITE_BASE}/api/runtime.html#context-providers`}>
          AgentSession &amp; ChatClient page
        </a>
        .
      </>
    ),
    s8Eyebrow: "Packaging — §8c",
    s8Heading: "Instructions need no plugin; code does",
    s8Body: (
      <>
        A skill that is pure instructions and references is mounted directly, no component
        and no loader, and that path is real today: <code>skill_tool_scoping.hpp</code>{" "}
        enforces <code>allowed-tools</code> as a genuine run-frozen restriction on top of{" "}
        <code>Capabilities&lt;...&gt;</code>, not just advisory text, when a caller passes it
        through consistently to <code>invoke_tool()</code>. A skill that ships{" "}
        <strong>code</strong> is meant to package as an <code>ae:skill</code> plugin and
        inherit the whole trust pipeline — signed, capability-declaring, operator-approved,
        digest-pinned, revocable. That packaging path doesn't exist yet; see Status below.
      </>
    ),
    s8Note: (
      <>
        007 §3 — <code>Capabilities&lt;...&gt;</code>. 009 §3 — the <code>ae:skill</code>{" "}
        plugin world.
      </>
    ),
    s9Eyebrow: "§8f — the five skills the engine ships itself",
    s9Heading: (
      <>
        First-party <code>SKILL.md</code> bundles, no special loader
      </>
    ),
    s9Body: (
      <>
        These earn their place precisely because a model hasn't seen a million examples of them,
        unlike ordinary Python. Shipped in this repo, mounted by default, subject to the same
        per-session grant model as any other skill. Field-by-field detail, the tools they pair
        with, and a worked example live on the{" "}
        <a href={`${SITE_BASE}/api/builtin-tools.html`}>First-Party Tools &amp; Skills page</a>.
      </>
    ),
    skillsTableColumns: ["Skill", "Teaches"],
    statusNote: (
      <>
        <strong>Status: the format, sourcing, and mounting are real — packaging code as a
        skill isn't, yet.</strong> <code>parse_skill_md</code> (<code>skill.hpp</code>)
        validates <code>SKILL.md</code> frontmatter with a distinct error code per rule;{" "}
        <code>SkillsProvider</code> (<code>skill_provider.hpp</code>) mounts resolved skills
        read-only through the real Tree/Ref/Mount machinery and contributes each one's
        name/description advertisement to the model's context; <code>skill_tool_scoping.hpp</code>{" "}
        enforces <code>allowed-tools</code> for real. Nine tests cover parsing, disk/inline
        sourcing, mounting, on-demand mount, and two <code>AgentSession</code> skills
        end-to-end suites. What's still design-only: the <code>ae:skill</code> WASM Component
        Model world for skills that ship code. <code>wit/README.md</code> still lists it{" "}
        <em>"not yet authored"</em> — <code>ae:tool</code>, by contrast, is real; see the
        Plugins page. A skill that bundles executable code beyond what the shell/Python tools
        already run has no packaging, signing, or loader path yet.
      </>
    ),
  },
  vi: {
    eyebrow: "009 §8 — Plugin and Extension System",
    headingPrefix: "Skill: năng lực được mount qua filesystem,",
    headingHighlight: "không phải một lệnh gọi tool",
    intro: (
      <>
        Một <strong>skill</strong> đóng gói các chỉ dẫn, tài nguyên, và script tùy chọn, trao
        cho agent một năng lực theo yêu cầu — khác với một <code>Tool</code>: không có tham
        số định kiểu, không có <code>invoke()</code>, không có lệnh gọi bị kiểm soát bởi
        capability. AgentEngine dùng <code>SKILL.md</code> (chuẩn mở <code>agentskills.io</code>,
        Apache-2.0) làm định dạng chính thức — đúng bố cục mà các sản phẩm của Anthropic và
        Microsoft Agent Framework đã dùng. Định dạng, cách lấy nguồn, và cách mount bên dưới
        đều có thật và đã kiểm thử; phần duy nhất vẫn còn ở dạng thiết kế được nêu riêng ở
        phần bên dưới.
      </>
    ),
    recoLabel: "Mặc định khuyến nghị",
    recoBody: (
      <>
        Chỉ hai trường frontmatter là bắt buộc — <code>name</code> và <code>description</code>;{" "}
        <code>parse_skill_md</code> từ chối một tài liệu thiếu một trong hai với mã lỗi riêng
        của nó. <code>license</code>, <code>compatibility</code>, <code>metadata</code>, và{" "}
        <code>allowed-tools</code> đều tùy chọn và có thể bỏ hẳn. Một skill tối giản chỉ cần
        đúng hai trường này cộng với phần thân Markdown:
      </>
    ),
    treeNote: (
      <>
        <strong>Tên skill là nhãn, không phải định danh.</strong> Skill được đặt namespace
        theo từng nguồn (origin) để một skill lấy từ nguồn từ xa không bao giờ có thể che
        khuất (shadow) một skill cục bộ.
      </>
    ),
    tableColumns: ["Trường frontmatter", "Kiểu", "Bắt buộc", "Ghi chú"],
    s1Eyebrow: "core/skill_source.hpp — một skill đến từ đâu",
    s1Heading: (
      <>
        Hai cách triển khai <code>SkillSource</code> có thật, cùng một concept
      </>
    ),
    s1Body: (
      <>
        Type-erased lúc chạy, không phải một gói template lúc biên dịch. Một session khai
        báo sẽ nạp skill từ những nguồn nào như một cấu hình lúc chạy bình thường — đường dẫn
        đĩa, bundle nội tuyến — đúng hình dạng mà <code>ToolTable</code> vốn đã chấp nhận cho
        tool. Cả hai nguồn thật bên dưới đều thỏa mãn cùng một concept <code>SkillSource</code>{" "}
        và được bọc giống hệt nhau bởi <code>make_skill_source_descriptor</code> thành{" "}
        <code>SkillSourceDescriptor</code> type-erased mà một <code>SkillsProvider</code> giữ
        một danh sách. Một nguồn thứ ba, chẳng hạn một registry từ xa, sẽ chỉ cần đúng hai
        phương thức đó.
      </>
    ),
    s1Note: (
      <>006 §6 — <code>ToolTable</code> chấp nhận đúng hình dạng cấu hình lúc chạy này cho tool.</>
    ),
    s2Eyebrow: "InlineSkillSource, đầy đủ",
    s2Heading: "Toàn bộ lớp chỉ có mười một dòng — đây là từng dòng một",
    s2Body: (
      <>
        <code>InlineSkillSource</code> cố ý là kiểu tuân theo <code>SkillSource</code> đơn
        giản nhất có thể: hai constructor lưu lại những gì chúng được trao, và hai accessor
        trả chúng lại. Không phân tích, không I/O, không có cache nào để hết hạn — caller làm
        hết công việc, thường là qua <code>parse_skill_md</code> áp lên một string literal,
        trước khi kiểu này từng nhìn thấy nó. Constructor thứ hai nhận thẳng một{" "}
        <code>result&lt;std::vector&lt;SkillSourceResult&gt;&gt;</code>, để một caller mà
        việc phân tích lúc khởi tạo của chính nó có thể thất bại vẫn có thể trao cho lớp này
        một thất bại và nhận lại đúng thất bại đó, không đổi, từ <code>load_skills()</code>.
        Năm skill tích hợp sẵn của <code>builtin_skills.hpp</code> và skill{" "}
        <code>extracting-document-text</code> của danh mục PDF-tool đều đi theo đường này.
      </>
    ),
    s2ExampleIntro: (
      <>
        Một ví dụ minh họa — xây một skill hoàn toàn trong bộ nhớ và trao nó cho một{" "}
        <code>SkillsProvider</code> giống hệt cách một skill lưu trên đĩa được trao, không có
        filesystem nào tham gia ở bất kỳ điểm nào:
      </>
    ),
    s2FailureIntro: (
      <>
        Trọng tâm của constructor thứ hai, tách riêng ra: trao cho nó một thất bại thay vì một
        danh sách skill thật, và nhận lại đúng thất bại đó — không phải một "load thất bại"
        chung chung, không phải một danh sách rỗng để rồi âm thầm xử lý vòng qua. Bất kể lỗi gì
        mà việc phân tích cú pháp lúc khởi tạo của caller tạo ra, đó chính là những gì một lời
        gọi <code>load_skills()</code> sau đó nhìn thấy, không đổi, mỗi lần — và điều đó vẫn
        đúng ngay cả sau khi <code>make_skill_source_descriptor</code> xóa bỏ kiểu cụ thể:
      </>
    ),
    s2EagerParseIntro: (
      <>
        Và đây là caller thật sự mà nó tồn tại để phục vụ — nguồn skill{" "}
        <code>extracting-document-text</code> của chính danh mục PDF-tool, nguyên văn. Việc
        phân tích cú pháp diễn ra đúng một lần, bên trong lambda được gọi ngay lập tức, trước
        khi <code>InlineSkillSource</code> từng được khởi tạo; bản thân lớp này không bao giờ
        phân tích bất cứ thứ gì, nó chỉ phát lại đúng <code>result</code> mà nó được trao:
      </>
    ),
    s2Note: (
      <>
        <strong>Vì sao nó tồn tại, không chỉ là cách nó hoạt động:</strong> năm skill tích
        hợp sẵn mà engine này phát hành — <code>using-the-code-interpreter</code>,{" "}
        <code>using-codeact</code>, và các skill khác — được biên dịch thẳng vào binary dưới
        dạng string literal thô. <code>builtin_skills.hpp</code> phân tích mỗi cái theo đúng
        cách đó, tại thời điểm khởi tạo, để một deployment không bao giờ có thể vô tình bỏ sót
        hay di chuyển một file <code>SKILL.md</code> rời rạc mà một skill lõi phụ thuộc vào.
        Nó khởi tạo một <code>InlineSkillSource</code>, đúng đường mà skill{" "}
        <code>extracting-document-text</code> của chính danh mục PDF-tool đi, thay vì tự tay
        lắp <code>SkillSourceDescriptor</code> type-erased: mọi nguồn skill biên dịch sẵn mà
        engine này phát hành đều đi qua đúng một lớp được xây dựng cho chính việc đó. Việc kết
        hợp nhiều descriptor dựa trên <code>InlineSkillSource</code> cạnh nhau — hai origin độc
        lập trong cùng một <code>SkillsProvider</code>, cộng với vòng đời mount theo yêu cầu mở
        khóa tool của chúng — được trình bày ở phần xa hơn bên dưới, với các đoạn mã riêng, tại{" "}
        <a href="#skill-loading">cách một skill được nạp</a>.
      </>
    ),
    s3Eyebrow: "Một skill được nạp như thế nào — §8b",
    s3Heading: "Ba tầng tiết lộ, tất cả đều chỉ là các phép đọc filesystem",
    step1Title: "Luôn được quảng cáo",
    step1Body: (
      <>
        <code>name</code> + <code>description</code> của mỗi skill đã mount — khoảng 100
        token — luôn hiển thị ngay từ đầu, cho mọi skill, bất kể kích thước danh mục.
      </>
    ),
    step2Title: "Nạp khi được kích hoạt",
    step2Body: (
      <>
        Phần thân <code>SKILL.md</code> (mục tiêu &lt;5.000 token) chỉ nạp một khi agent
        quyết định skill này áp dụng được.
      </>
    ),
    step3Title: "Nạp theo yêu cầu",
    step3Body: (
      <>
        Các thư mục <code>scripts/</code>, <code>references/</code>, và{" "}
        <code>assets/</code> đi kèm chỉ được đọc khi các chỉ dẫn ở bước 02 bảo agent làm
        vậy. stdout của một script đi kèm đi vào context — mã nguồn của nó thì không.
      </>
    ),
    s3Note: (
      <>
        Một skill được mount chỉ-đọc tại <code>/skills/&lt;name&gt;</code> và agent đọc nó
        bằng các phép thao tác file bình thường — <strong>không tồn tại wrapper tool{" "}
        <code>load_skill</code> / <code>read_skill_resource</code> nào; mount chính là cơ
        chế.</strong>{" "}
        Đây là một khác biệt cố ý so với Microsoft Agent Framework, vốn dùng đúng hai tool đó
        (cộng thêm <code>run_skill_script</code>) như một bề mặt ba-tool cố định bất kể kích
        thước danh mục. Việc nạp là động nhưng được chụp nhanh (snapshot) theo từng run: một
        skill được nạp giữa run không hồi tố thay đổi những gì các lượt trước đó được phép
        làm.
      </>
    ),
    s4Eyebrow: "core/skill_provider.hpp — mounting",
    s4Heading: (
      <>
        <code>SkillsProvider</code> — một <code>ContextProvider</code> thật, không phải stub
      </>
    ),
    s4Body: (
      <>
        Phân giải mọi nguồn đã khai báo, mount mỗi skill đã phân giải ở chế độ chỉ-đọc thông
        qua cơ chế Tree/Ref/Mount thật của <code>worktree.hpp</code>, và đóng góp một thông
        điệp quảng cáo <code>role::system</code> nêu tên mọi skill đã mount.{" "}
        <code>Mount.mount_id</code> của một skill đã mount là tên trần, ví dụ{" "}
        <code>"using-codeact"</code>, không phải chuỗi <code>"/skills/&lt;name&gt;"</code>{" "}
        theo nghĩa đen. <code>mount_id</code> thuần túy là khóa để so khớp capability mà{" "}
        <code>cap::FsRead</code> và <code>cap::FsWrite</code> đối chiếu vào, đó chính là điều
        cho phép một operator có được độ chi tiết thật theo từng skill: quyền đọc có thể được
        cấp cho file của một skill mà không phơi bày mọi skill khác đã mount. Đường dẫn logic{" "}
        <code>/skills/&lt;name&gt;</code> không bị ảnh hưởng bởi lựa chọn này; nó chỉ chạm
        tới model thông qua thông điệp quảng cáo.
      </>
    ),
    flowInlineSub: "văn bản SKILL.md đóng gói sẵn",
    flowDiskSub: "một thư mục host thật",
    flowResolveArrow: "resolve_and_mount() — bất kỳ xung đột nào cũng làm toàn bộ lệnh gọi thất bại đóng",
    flowWorktreeSub: "mỗi skill đã phân giải được mount chỉ-đọc tại /skills/<name>",
    flowOnContext: "on_context()",
    flowAdMessageSub: "nêu tên mọi skill đã mount — ~100 token, bất kể kích thước danh mục",
    s4Note: (
      <>
        <strong>Chống che khuất (anti-shadowing) là một sự từ chối, không bao giờ là
        nguồn-cuối-cùng-thắng.</strong> Được xây hoàn toàn vào các vector cục bộ và chỉ được
        gán vào trạng thái nền của <code>mounted()</code> khi toàn bộ thành công, một xung đột
        ở bất cứ đâu trong vòng lặp làm toàn bộ lệnh gọi <code>on_context()</code> thất bại
        đóng — để lại không skill nào được mount, chứ không chỉ những skill tình cờ được xử lý
        trước.
      </>
    ),
    s4TwoSourcesIntro: (
      <>
        Để đối chiếu, trường hợp thành công: hai skill có tên khác nhau từ hai nguồn khác
        nhau mount cùng nhau mà không hề có xung đột nào.
      </>
    ),
    s5Eyebrow: "core/skill_tool_scoping.hpp — thực thi §8c",
    s5Heading: (
      <>
        <code>allowed-tools</code> trở thành một hạn chế có thật — nếu cả hai phía luôn đồng
        bộ
      </>
    ),
    s5Body: (
      <>
        <code>scope_tools_to_mounted_skills()</code> lọc một <code>ToolTable</code> xuống
        còn các tên mà caller cho phép, thường là <code>SkillsProvider::allowed_tool_names()</code>{" "}
        hợp với một tập nền luôn-bật. Thực thi thật đòi hỏi áp dụng bộ lọc đó trên cả hai phía
        của ranh giới: những gì được khai báo cho model, và những gì{" "}
        <code>invoke_tool()</code> thực sự ủy quyền tại thời điểm gọi. Chỉ lọc phía khai báo
        và để lại một bảng rộng hơn tại điểm gọi thực thi chỉ mang tính hình thức — một tool
        bị "ẩn" khỏi danh sách của model vẫn gọi được nếu bảng tại thời điểm gọi vẫn còn chứa
        nó. Kiểm tra tại thời điểm gọi phải là kiểm tra thật.
      </>
    ),
    s5Note: (
      <>
        <code>ContextContribution.tools</code>, được tính mỗi lượt trong{" "}
        <code>on_context</code>, là phía khai báo; <code>invoke_tool()</code> là phía thực
        thi. I3 — đầu ra của model là dữ liệu, không bao giờ tự nó là một quyết định ủy quyền.
      </>
    ),
    s6Eyebrow: "Phụ lục Phase 3 — ADR-024",
    s6Heading: '"Đã phân giải" và "đã mount" là hai mối quan tâm độc lập',
    s6Body: (
      <>
        Mọi skill đã cấu hình đều được <strong>phân giải</strong> vô điều kiện khi session
        bắt đầu: file của nó được vật chất hóa (materialize) và <code>cap::FsRead</code> của
        nó đã được cấp, bất kể điều gì xảy ra sau đó. <code>MountedSkillsState</code> theo
        dõi một tập con hẹp hơn, do agent kích hoạt — skill đã phân giải nào hiện đang{" "}
        <strong>được mount</strong>, nghĩa là được khai báo cho model và bơm lại vào context.
        Mount một skill không cấp thêm quyền hạn nào mới; nó chỉ kích hoạt khả năng nhìn thấy
        capability vốn đã được cấp phát vô điều kiện từ trước. Đây là một tập hợp thay đổi
        được (mutable) cố ý đơn giản, không phải một token: <code>mount()</code> là
        idempotent, nên một agent không chắc về trạng thái của một skill có thể gọi lại nó
        miễn phí.
      </>
    ),
    s6Note: (
      <>
        I3 — đầu ra của model là dữ liệu, không bao giờ tự nó là một quyết định ủy quyền. Việc
        phân giải cấp <code>cap::FsRead</code> đúng một lần khi session bắt đầu; mount chỉ
        chuyển đổi những gì được khai báo cho model.
      </>
    ),
    s6ExampleIntro: (
      <>
        Pipeline thật, không chỉ là mô tả về nó: <code>examples/11_skill_mount.cpp</code> gọi
        cùng một lệnh gọi tool <code>word_count</code> hai lần vào cùng một pipeline{" "}
        <code>invoke_tool()</code>. Nó bị từ chối khi <code>word-counter</code> chưa được
        mount, được chấp nhận một khi đã mount — không có gì khác trong lệnh gọi thay đổi:
      </>
    ),
    s6CliNote: (
      <>
        <code>tools/cli_chat.cpp</code> là đối tác thật, tương tác trực tiếp: một model thật gọi{" "}
        <code>mount_skill</code> giữa cuộc hội thoại — chính tool mà mục này mô tả, được hiện ra
        qua đúng các event <code>tool_call_started</code>/<code>tool_call_finished</code> (
        <a href={`${SITE_BASE}/api/events.html`}>trang Events</a>) dưới dạng{" "}
        <code>"-&gt; calling tool 'mount_skill'"</code> / <code>"&lt;- tool call OK"</code>, cùng
        một banner khởi động phân biệt skill đã resolve, đã materialize, và đang mounted trước khi
        bất kỳ điều gì trong số đó xảy ra. Cần một credential model thật — xem điều kiện build
        ngay trong file.
      </>
    ),
    s7Eyebrow: "core/composed_context_provider.hpp — đấu nối vào AgentSession",
    s7Heading: (
      <>
        Một slot <code>HistoryProviderT</code>, hai bên đóng góp — và một lỗi thứ tự có
        thật mà test giờ đã chốt lại
      </>
    ),
    s7Body: (
      <>
        <code>AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;</code> có đúng một
        slot history-provider. <code>ComposedContextProvider&lt;H, S&gt;</code> kết hợp một
        history provider và một <code>SkillsProvider</code> vào slot duy nhất đó thông qua{" "}
        <code>assemble_context</code>, được engage một lần qua constructor (hoặc một lời gọi{" "}
        <code>engage()</code> tường minh) — không bao giờ xây lại bên trong{" "}
        <code>on_context()</code>, nếu không đảm bảo resolve-once/freeze của{" "}
        <code>SkillsProvider</code> sẽ âm thầm bị phá vỡ ở mỗi lượt.
      </>
    ),
    wrongLabel: "Sai — đẩy history trước",
    wrong: "Thứ tự trên dây: [...toàn bộ cuộc hội thoại..., quảng cáo skill]. Thông điệp quảng cáo hệ thống nằm sau mọi lượt trước đó, ngược lại với vị trí đặt system-prompt thông thường. Một bản dump dây thật xác nhận thứ tự này làm giảm mức tuân thủ của model đối với thông điệp quảng cáo.",
    rightLabel: "Đúng — đẩy skill trước",
    right: (
      <>
        Thứ tự trên dây: [quảng cáo skill, ...toàn bộ cuộc hội thoại...]. Thứ tự contributor
        chính là thứ tự trên dây, quy tắc riêng của <code>assemble_context</code>, nên khai
        báo skill trước history trong constructor là toàn bộ cách khắc phục.
      </>
    ),
    s7Note: (
      <>
        <strong>Thứ tự quan trọng, và phiên bản đầu tiên đã làm ngược.</strong> Các
        contributor được nối theo thứ tự khai báo. Việc đẩy history trước đặt thông điệp
        quảng cáo hệ thống của skill sau toàn bộ cuộc hội thoại trên mọi request thật, ngược
        lại với vị trí đặt system-prompt thông thường và đúng loại điều làm giảm mức tuân thủ
        của model. Một bản dump dây thật xác nhận hiệu ứng này; đó không phải giả định. Skill
        giờ được đẩy trước, và <code>test_agent_session_skills_real_backend.cpp</code> khẳng
        định thứ tự thông điệp, không chỉ sự hiện diện, nên điều này không thể âm thầm thoái
        lui.
      </>
    ),
    s7Trailing: (
      <>
        <code>ComposedContextProvider&lt;Ms...&gt;</code> là type composite tổng quát duy nhất
        cho bất kỳ N <code>ContextProvider</code> thật nào (history, skill, memory, …) trong một
        slot, vẫn dùng <code>assemble_context</code> bên dưới — khai báo <code>Ms...</code> theo
        đúng thứ tự trên dây bạn muốn, không cần một composite viết tay riêng nữa. Được đi qua
        chi tiết hơn ở{" "}
        <a href={`${SITE_BASE}/api/runtime.html#context-providers`}>
          trang AgentSession &amp; ChatClient
        </a>
        .
      </>
    ),
    s8Eyebrow: "Đóng gói — §8c",
    s8Heading: "Chỉ dẫn thì không cần plugin; mã thì cần",
    s8Body: (
      <>
        Một skill thuần chỉ dẫn và tài liệu tham khảo được mount trực tiếp, không component,
        không loader, và đường đó có thật ngay hôm nay: <code>skill_tool_scoping.hpp</code>{" "}
        thực thi <code>allowed-tools</code> như một hạn chế bị đóng băng theo run thật, xây
        trên nền <code>Capabilities&lt;...&gt;</code>, không chỉ là văn bản khuyến nghị, khi
        một caller truyền nó đi một cách nhất quán tới <code>invoke_tool()</code>. Một skill
        mang theo <strong>mã</strong> được dự định đóng gói thành một plugin{" "}
        <code>ae:skill</code> và kế thừa toàn bộ pipeline tin cậy — đã ký, khai báo
        capability, được operator phê duyệt, ghim theo digest, có thể thu hồi. Đường đóng gói
        đó chưa tồn tại; xem phần Trạng thái bên dưới.
      </>
    ),
    s8Note: (
      <>
        007 §3 — <code>Capabilities&lt;...&gt;</code>. 009 §3 — world plugin{" "}
        <code>ae:skill</code>.
      </>
    ),
    s9Eyebrow: "§8f — năm skill mà chính engine phát hành",
    s9Heading: (
      <>
        Các bundle <code>SKILL.md</code> chính chủ, không loader đặc biệt
      </>
    ),
    s9Body: (
      <>
        Chúng xứng đáng có vị trí này chính xác vì một model chưa từng thấy hàng triệu ví dụ về
        chúng, khác với Python thông thường. Được phát hành trong repo này, mount theo mặc định,
        chịu cùng mô hình cấp phát theo từng session như bất kỳ skill nào khác. Chi tiết theo
        từng trường, các tool đi kèm, và một ví dụ minh họa nằm ở{" "}
        <a href={`${SITE_BASE}/api/builtin-tools.html`}>trang Tool &amp; Skill chính chủ</a>.
      </>
    ),
    skillsTableColumns: ["Skill", "Dạy điều gì"],
    statusNote: (
      <>
        <strong>Trạng thái: định dạng, cách lấy nguồn, và mounting đều có thật — đóng gói mã
        thành một skill thì chưa, ít nhất là chưa tính đến giờ.</strong>{" "}
        <code>parse_skill_md</code> (<code>skill.hpp</code>) xác thực frontmatter của{" "}
        <code>SKILL.md</code> với một mã lỗi riêng cho mỗi quy tắc; <code>SkillsProvider</code>{" "}
        (<code>skill_provider.hpp</code>) mount các skill đã phân giải ở chế độ chỉ-đọc thông
        qua cơ chế Tree/Ref/Mount thật và đóng góp thông điệp quảng cáo name/description của
        mỗi skill vào context của model; <code>skill_tool_scoping.hpp</code> thực thi{" "}
        <code>allowed-tools</code> một cách thật. Chín test bao phủ việc phân tích, lấy
        nguồn đĩa/nội tuyến, mounting, mount theo yêu cầu, và hai bộ test đầu-cuối skill của{" "}
        <code>AgentSession</code>. Phần vẫn còn ở dạng thiết kế: world{" "}
        <code>ae:skill</code> của WASM Component Model dành cho skill mang theo mã.{" "}
        <code>wit/README.md</code> vẫn liệt kê nó là <em>"chưa được viết"</em> — trong khi{" "}
        <code>ae:tool</code> đã có thật; xem trang Plugin. Một skill đóng gói mã thực thi vượt
        ra ngoài những gì các tool shell/Python đã chạy hiện chưa có đường đóng gói, ký, hay
        loader nào.
      </>
    ),
  },
} as const;

export function ApiSkillReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" id="skills">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {tu.statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        <RevealGroup>
          <RevealItem>
            <div className="gs-recommend">
              <span className="gs-recommend-label">{t.recoLabel}</span>
              <p>{t.recoBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="quick-note-format/SKILL.md">{minimalSkillSnippet}</CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="spec-layout">
          <RevealGroup>
            <RevealItem>
              <CodePanel filename="skill-name/">{directoryTree}</CodePanel>
            </RevealItem>
            <RevealItem>
              <p className="gs-note" style={{ marginTop: 20 }}>{t.treeNote}</p>
            </RevealItem>
          </RevealGroup>

          <RevealGroup>
            <RevealItem>
              <ApiTable
                columns={[...t.tableColumns]}
                templateColumns="1fr 1.1fr 0.7fr 2.2fr"
                rows={skillFrontmatterFields[lang].map((f) => [
                  <code key="name">{f.name}</code>,
                  f.type,
                  f.required ? tu.required : tu.optional,
                  f.notes,
                ])}
              />
            </RevealItem>
          </RevealGroup>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-source">
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
              <ApiDiagnosticNote>{t.s1Note}</ApiDiagnosticNote>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="doc-entries">
              {skillSourceEntries[lang].map((e) => (
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
            <CodePanel filename="skill_source.hpp">{highlightCpp(skillSourceConceptSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="inline-skill-source-detail">
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="skill_source.hpp">{highlightCpp(inlineSkillSourceShapeSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 20, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.s2ExampleIntro}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="test_skill_source_inline.cpp">
              {highlightCpp(inlineSkillSourceExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 28, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.s2FailureIntro}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="test_skill_source_inline.cpp">
              {highlightCpp(inlineSkillSourceFailureConstructorSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 28, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.s2EagerParseIntro}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="tools/extract_pdf_text.hpp">
              {highlightCpp(inlineSkillSourceEagerParseSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s2Note}</p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-loading">
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "10px 18px", borderRadius: "var(--radius-lg)" }}>
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
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24 }}>{t.s3Note}</p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-mounting">
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow glass">
              <div className="flow-row">
                <div className="flow-node is-purple">
                  <div className="flow-node-title">InlineSkillSource</div>
                  <div className="flow-node-sub">{t.flowInlineSub}</div>
                </div>
                <div className="flow-node is-purple">
                  <div className="flow-node-title">DiskSkillSource</div>
                  <div className="flow-node-sub">{t.flowDiskSub}</div>
                </div>
              </div>
              <div className="flow-arrow">{t.flowResolveArrow}</div>
              <div className="flow-node">
                <div className="flow-node-title">worktree.hpp Tree / Ref / Mount</div>
                <div className="flow-node-sub">{t.flowWorktreeSub}</div>
              </div>
              <div className="flow-arrow">{t.flowOnContext}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">one role::system advertisement message</div>
                <div className="flow-node-sub">{t.flowAdMessageSub}</div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="skill_provider.hpp">{highlightCpp(skillsProviderApiSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24 }}>{t.s4Note}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="skill_provider.hpp — resolve_and_mount()">
              {highlightCpp(skillCollisionSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 20, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.s4TwoSourcesIntro}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="test_skill_provider_mount.cpp">
              {highlightCpp(skillTwoSourcesMountSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-tool-scoping">
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
              <ApiDiagnosticNote>{t.s5Note}</ApiDiagnosticNote>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="skill_tool_scoping.hpp">{highlightCpp(skillToolScopingSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-on-demand">
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
              <ApiDiagnosticNote>{t.s6Note}</ApiDiagnosticNote>
            </div>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 8, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.s6ExampleIntro}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/11_skill_mount.cpp">
              {highlightCpp(skillOnDemandMountSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <div style={{ marginTop: 20 }}>
              <ApiDiagnosticNote>{t.s6CliNote}</ApiDiagnosticNote>
              <a
                className="api-cite"
                href={gh("tools/cli_chat.cpp")}
                target="_blank"
                rel="noreferrer"
                style={{ display: "block", marginTop: 8 }}
              >
                tools/cli_chat.cpp →
              </a>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-composition">
              <span className="eyebrow">{t.s7Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s7Heading}</h3>
              <p>{t.s7Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.wrongLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.wrong}</p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.rightLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.right}</p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24, borderLeftColor: "var(--accent-pink)" }}>{t.s7Note}</p>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 14, color: "var(--text-dim)", lineHeight: 1.65 }}>{t.s7Trailing}</p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-packaging">
              <span className="eyebrow">{t.s8Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s8Heading}</h3>
              <p>{t.s8Body}</p>
              <ApiDiagnosticNote>{t.s8Note}</ApiDiagnosticNote>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-builtin">
              <span className="eyebrow">{t.s9Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s9Heading}</h3>
              <p>{t.s9Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.skillsTableColumns]}
              templateColumns="1.4fr 2.6fr"
              rows={genericSkills[lang].map((s) => [<code key="name">{s.name}</code>, s.teaches])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div
              className="gs-note anchor-target"
              id="skill-status"
              style={{ marginTop: 40, borderLeftColor: "var(--accent-pink)" }}
            >
              {t.statusNote}
              <div style={{ marginTop: 8, display: "flex", gap: 16, flexWrap: "wrap" }}>
                <a href={gh("include/agentengine/core/skill_provider.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/core/skill_provider.hpp
                </a>
                <a href={gh("include/agentengine/core/skill_tool_scoping.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/core/skill_tool_scoping.hpp
                </a>
                <a href={gh("wit/README.md")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  wit/README.md
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
