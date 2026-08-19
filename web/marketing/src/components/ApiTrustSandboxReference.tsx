import {
  attenuationExampleSnippet,
  capabilityDenialExampleSnippet,
  capabilityEnforcementSteps,
  customBackendConceptSnippet,
  gh,
  minimalCapabilitiesSnippet,
  nativeJailProcessLaunchSnippet,
  remoteCallbackAuthSnippet,
  remoteCallbackAuthSteps,
  sandboxAbuseCases,
  sandboxBackendMechanicsRows,
  sandboxBackends,
  sandboxDeterminismRows,
  sandboxDeterminismSnippet,
  sandboxJobTimeRuns,
  sandboxLifetimeSnippet,
  sandboxLifetimeSteps,
  sandboxObservabilityFields,
  sandboxPassivationRows,
  sandboxProfileSnippet,
  sandboxRunEventSnippet,
  toolPipelineSteps,
  trustEntries,
  wasmImportGatingSnippet,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";
import type { Lang } from "../i18n/LanguageContext";

function entryById(lang: Lang, id: string) {
  const e = trustEntries[lang].find((entry) => entry.id === id);
  if (!e) throw new Error(`missing trust entry: ${id}`);
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

// Plain external citation, for the §5-§8 sections below — these don't have a matching trustEntries
// id (that Record<Lang, ApiEntry[]> only covers §2a/§4/§4a), so this renders a link directly rather
// than routing through entryById/CiteLink. File paths and RFC/ADR section labels are language-
// neutral, so this needs no lang plumbing.
function RawCite({ href, label }: { href: string; label: string }) {
  return (
    <a
      className="api-cite"
      href={href}
      target="_blank"
      rel="noreferrer"
      style={{ borderTop: "none", paddingTop: 0 }}
    >
      {label}
    </a>
  );
}

const copy = {
  en: {
    eyebrow: "007/008 — Trust & Isolation (L1)",
    headingPrefix: "Capabilities are the only way to",
    headingHighlight: "reach an effect",
    intro: (
      <>
        I2 — no ambient authority — enforced by the type system, not by discipline: there is no
        constructor anywhere in this codebase that grants everything, only ones that narrow.
        Below is what that actually means for one tool call, from a tool's own declaration all
        the way to the sandbox that isolates its side effects.
      </>
    ),
    s1Eyebrow: "trust/capability.hpp",
    s1Heading: (
      <>
        A tool's <code>Capabilities&lt;...&gt;</code> is a ceiling, never a grant
      </>
    ),
    s1Body: (
      <>
        <code>WriteNoteTool</code> below declares it might need{" "}
        <code>FsWrite&lt;"work"&gt;</code>. Declaring that changes nothing about whether the
        tool can actually run — the only thing that authorizes a call is what the{" "}
        <em>session</em> was explicitly handed.
      </>
    ),
    s1RecoLabel: "Recommended default",
    s1RecoBody: (
      <>
        Before reaching for <code>Capabilities&lt;...&gt;</code> at all: most tools touch no
        privileged effect and declare NOTHING here, matching 007 §3's empty-by-default rule —{" "}
        <code>declared_capabilities()</code> already returns an empty ceiling unless overridden.
        Add exactly the <code>cap::decl::*</code> tag matching the one effect a tool actually
        reaches (a single <code>FsWrite&lt;"work"&gt;</code>, say) — never a broader set "in
        case it's needed later":
      </>
    ),
    declaresLabel: "What the tool declares",
    declares: (
      <>
        <code>Capabilities&lt;cap::decl::FsWrite&lt;"work"&gt;&gt;</code> — a compile-time
        ceiling, read by the pipeline to know what to check. The tool author states what it
        needs; that statement grants nothing.
      </>
    ),
    grantedLabel: "What the session was granted",
    granted: (
      <>
        <code>CapabilitySet::grant_root(...)</code> — called by the HOST, never reachable
        from anything derived from model output (I3). This is the only thing step 4/7 of
        the pipeline actually checks against.
      </>
    ),
    outcomeNote: (
      <>
        <strong>The same tool, the same call, two different outcomes</strong> — the ONLY
        variable across the two sessions above is what <code>CapabilitySet</code> was handed
        to <code>set_capabilities()</code>. Denial is an ordinary tool error fed back to the
        model, not a run-level failure: the run still converges, it just never invoked{" "}
        <code>write_note</code>'s real body.
      </>
    ),
    s2Eyebrow: "tool_pipeline.hpp — invoke_tool()",
    s2Heading: "Every tool call, real or model-requested, crosses the same ten steps",
    s2Body: (
      <>
        006 §3's own numbering — <code>invoke_tool()</code> is the ONE function every native
        tool call goes through, whether it came from the model or a workflow node. Two of
        these ten are the actual enforcement gates; the rest are resolution, bookkeeping, or
        a documented no-op.
      </>
    ),
    handleNote: (
      <>
        A bound capability handle from step 4/7 is valid for exactly ONE call — revoked
        unconditionally at step 10, success or failure, before the result is even normalized.
        A handle from call N is unusable in call N+1 by construction, not convention (this is
        what a real regression proves, not just what a comment claims).
      </>
    ),
    s3Eyebrow: "CapabilitySet::attenuate()",
    s3Heading: "Deriving a narrower set is the only direction that compiles",
    s3Body: (
      <>
        A parent <code>CapabilitySet</code> can hand out a strictly narrower child — a
        delegate agent, a spawned sub-worktree, a plugin — but only ever narrower. Asking for
        anything the parent doesn't already cover fails the WHOLE derivation closed, not
        partially.
      </>
    ),
    s3Note: (
      <>
        <strong>No convenience "give me everything" shortcut exists.</strong>{" "}
        <code>CapabilitySet()</code> default-constructs empty; <code>grant_root()</code> is
        the ONE explicitly-named, greppable entry point host policy calls. That's not a
        comment promising a property — it's the actual thing 007 §9's own falsifiable test
        gate checks.
      </>
    ),
    s4Eyebrow: "sandbox/sandbox.hpp",
    s4Heading: (
      <>
        <code>SandboxProfile&lt;Strict&gt;</code> — the strongest backend for THIS platform
      </>
    ),
    s4Body: (
      <>
        A capability grant says WHAT an effect may reach; a sandbox profile is about HOW
        isolated the process running it is. <code>P</code> in{" "}
        <code>SandboxProfile&lt;P&gt;</code> is either a concrete backend or the{" "}
        <code>Strict</code> selector, resolved at build/startup time by ranking every backend
        that actually supports the current platform.
      </>
    ),
    strengthLabel: "strength",
    coldStartLabel: "cold start",
    s5Eyebrow: "008 §4 — Capability enforcement per backend",
    s5Heading: "Same table, two completely different mechanisms underneath",
    s5Body: (
      <>
        <code>native-jail</code> is OS-ACL-gated: a grant becomes a token attribute and a
        filesystem ACL, set up once at process launch, checked by the kernel on every access.{" "}
        <code>wasm</code> is host-function-gated: a grant becomes whether the linker defines a
        host callback for a WIT interface AT ALL — an ungranted capability isn't denied at
        call time, it never exists as something the component could call in the first place.
        Both are real, tested backends; the row below is read directly off their own source,
        not the RFC's summary of it.
      </>
    ),
    s5TableColumns: ["Axis", "native_jail", "wasm"],
    s5Finding: (
      <>
        <strong>
          The headline finding this table's third row is built on: AppContainer's ACL model is
          NOT a sufficient filesystem boundary by itself.
        </strong>{" "}
        A curated set of host files — <code>win.ini</code>,{" "}
        <code>drivers\etc\hosts</code> — carry{" "}
        <code>ALL APPLICATION PACKAGES</code>/<code>ALL RESTRICTED APPLICATION PACKAGES</code>{" "}
        read ACEs by Windows' own default, inherited, independent of whatever this backend
        grants or withholds (ADR-004 §6 finding 1). That is exactly why 008 §1b makes
        interpreter-level <code>open()</code> mediation the PRIMARY filesystem control and
        treats the kernel jail's ACL denial as backstop only — this finding is the concrete
        case that framing exists to answer, not a hypothetical risk.
      </>
    ),
    s5CompareBeforeLabel: "native_jail — one-time process launch",
    s5CompareAfterLabel: "wasm — per-load import gate",
    s5LadderIntro: (
      <>
        What happens on every single tool call inside a loaded component — ADR-010 §3.2's real
        bind/link/instantiate/revoke order, the same unconditional-revoke idiom 006 §3 step 10
        already uses for native tools, applied to a completely different backend:
      </>
    ),
    s6Eyebrow: "008 §2a — Custom backends",
    s6Heading: (
      <>
        <code>SandboxBackend</code> is a concept, not a closed enum of three profiles
      </>
    ),
    s6Body: (
      <>
        Nothing about the four profiles above is engine-private. Any type satisfying the same{" "}
        <code>SandboxBackend</code> concept — a deployer's own gVisor, Kata, or future
        microVM-backed wrapper — plugs into <code>SandboxProfile&lt;P&gt;</code> exactly the
        way <code>NativeJailBackend</code> or <code>WasmBackend</code> does, because from the
        engine's point of view there is no difference between them.
      </>
    ),
    s6Note: (
      <>
        <strong>
          The trade a deployer makes by supplying one: host-trust-tier code, not sandboxed
          code.
        </strong>{" "}
        A custom backend runs unsandboxed, the same trust tier as first-party native tools
        (007 §6 T0) — the engine does not attempt to contain the thing that creates and
        manages containment. It must still clear the same §9 promotion-gate bar the built-in
        profiles do (G1 outcome parity, G2 containment with a positive control, G3 no ambient
        authority, ...) — the engine cannot run that gate automatically on code it did not
        write, so this is the standard a custom backend is held to, not a check enforced for
        it, the identical posture 007 §7 already takes toward third-party plugin trust.
      </>
    ),
    s7Eyebrow: "008 §4a — RemoteExecToken",
    s7Heading: (
      <>
        A <code>remote</code> sandbox calling back to the host authenticates itself with a
        token it cannot forge and the host never has to look up
      </>
    ),
    s7Body: (
      <>
        The <code>remote</code> profile's mounts and network policy are materialized through
        its own control plane, out of band — but when the remote instance calls BACK to the
        host (secret resolution, a <code>ToolCall</code> dispatch, anything "egress is always
        host-mediated" routes through the host), the host needs to know which live{" "}
        <code>Exec</code> that callback belongs to, without syscall-mediating a process it
        doesn't own. <code>RemoteExecToken</code> is a self-verifying, macaroon-style bearer
        token — not an opaque lookup key into a host-side registry — built on the exact{" "}
        <code>mint_root</code>/<code>attenuate</code>/<code>verify</code> primitive ADR-005
        proved out for capabilities crossing a process boundary generally.
      </>
    ),
    s7StatusNote: (
      <>
        <strong>
          Status: the primitive is real and proven; the wiring isn't, because there's nothing
          to wire it to yet.
        </strong>{" "}
        <code>mint_root</code>/<code>attenuate</code>/<code>verify</code> in{" "}
        <code>trust/capability_token.hpp</code> are real, executed, red-teamed, ASan-clean
        code (ADR-005). <code>remote</code> itself is scaffolding only — no live backend in
        this codebase calls <code>mint_root()</code> today. §4a's decision is which mechanism
        the <code>remote</code> profile will use once it exists, grounded in a primitive that
        already works, not a claim that the callback path is wired end to end.
      </>
    ),
    s8Eyebrow: "008 §5 — Determinism and replay",
    s8Heading: (
      <>
        Only <code>wasm</code> is a pure function of its inputs — the others are honest about
        what replay actually serves
      </>
    ),
    s8Body: (
      <>
        <code>Determinism</code> is two bools on <code>SandboxSpec</code>, not a claim the
        other profiles quietly inherit. Turning both on for <code>wasm</code>, combined with
        content-addressed inputs (003 §3), makes one exec a pure function of{" "}
        <code>(component, inputs, capabilities)</code> — cacheable by digest, replayable
        offline. <code>native-jail</code> and <code>remote</code> record instead: replay
        there serves the recorded output, it does not re-derive it.
      </>
    ),
    s9Eyebrow: "008 §6 — Lifetime, pooling, and state",
    s9Heading: "The boundary that matters is between sessions, not between execs within one",
    s9Body: (
      <>
        <code>sandbox_lifetime</code> is a real, plain three-way enum — no hidden fourth
        state. Cross-session reuse is prohibited in every profile; a pooled instance is
        always reset to a snapshot taken <em>before</em> any untrusted input, never merely
        "cleaned". None of this touches files: the worktree (025) is durable engine state
        mounted into the sandbox, so destroying a sandbox on any profile loses no files.
      </>
    ),
    s9aEyebrow: "008 §6a — surviving passivation",
    s9aHeading: "Files always survive it. In-memory interpreter state depends on the profile.",
    s9aTableColumns: ["", "wasm (plugins, 009)", "native-jail (interpreter and shell, 010)"],
    s9Note: (
      <>
        <strong>Named gap, not a silently-working feature.</strong> Quark previously
        passivated idle sessions (ADR-028/034); ADR-037 removed Quark, and no host-managed
        session-passivation mechanism currently exists in <code>agentengine::rt::</code>. The
        table above states the TARGET behavior for whenever passivation exists again — the{" "}
        <code>wasm</code> snapshot property itself is real (a component's heap is its linear
        memory at a quiescent point, so a snapshot is a memory dump plus store/table/global
        state), but there is no portable equivalent for native processes: CRIU is Linux-only
        and, by its own docs, frequently fails on live network connections. This is an
        accepted, permanent limitation for the interpreter and shell, not a gap to close by
        picking a different backend for them — §1 already rules out a WASM Python runtime for
        its own reasons.
      </>
    ),
    s10Eyebrow: "008 §7 — Failure and abuse",
    s10Heading: (
      <>
        A named hostile corpus, each case with a positive control — and two honestly-named
        gaps
      </>
    ),
    s10Body: (
      <>
        Every row below has a test in the hostile suite (resource-capped, per CLAUDE.md
        Machine Safety, so proving a fork bomb is contained can't take the dev box with it).
        Two rows are marked as a tracked gap rather than contained — this page says so
        because §9 G2's own promotion gate requires a positive control that "demonstrably
        fails", and a claim this page can't back with one doesn't belong here as "contained".
      </>
    ),
    s10TableColumns: ["Case", "Containment / status", ""],
    containedLabel: "Contained",
    namedGapLabel: "Named gap",
    s10aEyebrow: "ADR-004 §10.2 — 11 real runs, not a rounded-off claim",
    s10aHeading: "What Job Object limits actually guarantee, measured — not assumed",
    s10aBody: (
      <>
        A CPU-spinning child under <code>cpu_ms=500</code> with a <code>wall_ms=5000</code>{" "}
        backstop, run 11 times across 3 invocations of the same test binary, to see which
        mechanism actually fires.
      </>
    ),
    s10aTableColumns: ["Run", "Fired via JOB_OBJECT_LIMIT_JOB_TIME?", "CPU consumed", "Overrun vs. 500ms"],
    s10aFiredYes: "Yes",
    s10aFiredNo: "No — wall_ms backstop fired",
    s10aNote: (
      <>
        <strong>
          3 of 11 runs fired automatically via the kernel's own <code>JOB_OBJECT_LIMIT_JOB_TIME</code>
        </strong>
        , with overrun ranging <strong>1.38x–8.22x</strong> the configured budget and no
        discernible relationship between the configured limit and the actual overrun — the
        smallest and largest overruns are 6x apart. The other 8 of 11 didn't fire at all within
        the 5-second window, letting ~9.5x–9.8x the budget accumulate before the host-side{" "}
        <code>wall_ms</code> watcher (proven reliable elsewhere at 500–504ms for a 500ms
        deadline) terminated the job instead. <code>cpu_ms</code> is therefore documented as
        best-effort, not a dependable bound, on Windows <code>native-jail</code> — a caller
        that needs a real CPU/time bound sets <code>wall_ms</code> to the value it actually
        wants enforced. Memory and process-count limits are the opposite story: precise and
        fast (14–22ms) and exact (2 of 5 spawn attempts succeeded under a cap of 3), confirmed
        by their own positive controls. This is a per-backend reliability difference, not a
        contract violation — 008 §9 G1 requires identical outcome CLASSIFICATION across
        backends (a CPU-bound guest is always killed and always reported as
        resource-exceeded), never identical enforcement latency or mechanism.
      </>
    ),
    s11Eyebrow: "008 §8 — Observability",
    s11Heading: "The run-event vocabulary already names a sandbox exec as a first-class boundary",
    s11Body: (
      <>
        Per exec, 008 §8 specifies nine fields a host should be able to observe — enough
        that a profile downgrade shows up as a graph change in metrics, not a surprise in an
        incident review, because every metric carries the profile.
      </>
    ),
    s11TableColumns: ["Field", "What it means"],
    s11Note: (
      <>
        <strong>The shape exists; the producer doesn't yet.</strong>{" "}
        <code>sandbox_exec_started</code>/<code>sandbox_exec_finished</code> are real members
        of the one shared <code>run_event_kind</code> enum every protocol surface (AG-UI
        included) is meant to project from — but <code>run_event.hpp</code>'s own top comment
        names the gap plainly: <code>AgentSession</code>'s real turn loop fires
        Run/Turn/ModelCall/StateChanged events today, and makes exactly one synchronous,
        non-streaming model call that never reaches the tool pipeline — let alone a sandbox
        exec — at all yet. This page draws that line rather than implying a live stream of
        §8's nine fields exists today.
      </>
    ),
  },
  vi: {
    eyebrow: "007/008 — Trust & Isolation (L1)",
    headingPrefix: "Capability là cách duy nhất để",
    headingHighlight: "chạm tới một effect",
    intro: (
      <>
        I2 — không có quyền hạn mặc nhiên — được hệ thống kiểu thực thi, không dựa vào kỷ luật
        lập trình: không có constructor nào trong codebase này cấp toàn quyền, chỉ có những
        constructor thu hẹp lại. Bên dưới là ý nghĩa thực sự của điều đó đối với một lệnh gọi
        tool, từ chính khai báo của tool cho tới sandbox cách ly các side effect của nó.
      </>
    ),
    s1Eyebrow: "trust/capability.hpp",
    s1Heading: (
      <>
        <code>Capabilities&lt;...&gt;</code> của một tool là một ceiling, không bao giờ là
        một sự cấp phát
      </>
    ),
    s1Body: (
      <>
        <code>WriteNoteTool</code> bên dưới khai báo rằng nó có thể cần{" "}
        <code>FsWrite&lt;"work"&gt;</code>. Việc khai báo đó không thay đổi gì về việc tool có
        thực sự chạy được hay không — thứ duy nhất ủy quyền cho một lệnh gọi là những gì{" "}
        <em>session</em> được trao một cách tường minh.
      </>
    ),
    s1RecoLabel: "Mặc định khuyến nghị",
    s1RecoBody: (
      <>
        Trước khi dùng tới <code>Capabilities&lt;...&gt;</code>: hầu hết tool không chạm tới
        effect đặc quyền nào cả và KHÔNG khai báo gì ở đây, đúng theo quy tắc mặc định-rỗng của
        007 §3 — <code>declared_capabilities()</code> đã trả về một ceiling rỗng trừ khi được
        ghi đè. Chỉ thêm đúng thẻ <code>cap::decl::*</code> khớp với effect duy nhất mà tool
        thực sự chạm tới (ví dụ một <code>FsWrite&lt;"work"&gt;</code> duy nhất) — không bao
        giờ khai báo rộng hơn "phòng khi sau này cần":
      </>
    ),
    declaresLabel: "Những gì tool khai báo",
    declares: (
      <>
        <code>Capabilities&lt;cap::decl::FsWrite&lt;"work"&gt;&gt;</code> — một ceiling tại
        thời điểm biên dịch, được pipeline đọc để biết cần kiểm tra gì. Người viết tool nêu
        ra nó cần gì; lời khai báo đó không cấp phát bất cứ điều gì.
      </>
    ),
    grantedLabel: "Những gì session được cấp",
    granted: (
      <>
        <code>CapabilitySet::grant_root(...)</code> — được HOST gọi, không bao giờ chạm tới
        được từ bất cứ thứ gì bắt nguồn từ đầu ra của model (I3). Đây là thứ DUY NHẤT mà bước
        4/7 của pipeline thực sự kiểm tra dựa vào.
      </>
    ),
    outcomeNote: (
      <>
        <strong>Cùng một tool, cùng một lệnh gọi, hai kết quả khác nhau</strong> — biến số
        DUY NHẤT giữa hai session ở trên là <code>CapabilitySet</code> nào được trao cho{" "}
        <code>set_capabilities()</code>. Từ chối là một lỗi tool bình thường được đưa trở lại
        cho model, không phải một thất bại ở cấp run: run vẫn hội tụ, chỉ là nó không bao giờ
        gọi thực thi phần thân thật của <code>write_note</code>.
      </>
    ),
    s2Eyebrow: "tool_pipeline.hpp — invoke_tool()",
    s2Heading: "Mọi lệnh gọi tool, dù thật hay do model yêu cầu, đều đi qua đúng mười bước",
    s2Body: (
      <>
        Đúng theo cách đánh số của 006 §3 — <code>invoke_tool()</code> là hàm DUY NHẤT mà mọi
        lệnh gọi tool gốc (native) đi qua, bất kể nó đến từ model hay từ một node workflow.
        Hai trong số mười bước này là các cổng thực thi thật; phần còn lại là phân giải, sổ
        sách (bookkeeping), hoặc một no-op đã được ghi nhận.
      </>
    ),
    handleNote: (
      <>
        Một capability handle đã ràng buộc từ bước 4/7 chỉ có hiệu lực cho đúng MỘT lệnh gọi
        — bị thu hồi vô điều kiện ở bước 10, dù thành công hay thất bại, trước cả khi kết quả
        được chuẩn hóa. Một handle từ lệnh gọi N không thể dùng được ở lệnh gọi N+1, do chính
        cấu trúc quyết định chứ không phải quy ước (đây là điều một hồi quy thật chứng minh,
        không chỉ là điều một comment tuyên bố).
      </>
    ),
    s3Eyebrow: "CapabilitySet::attenuate()",
    s3Heading: "Suy ra một tập hẹp hơn là hướng duy nhất biên dịch được",
    s3Body: (
      <>
        Một <code>CapabilitySet</code> cha có thể trao ra một tập con hẹp hơn một cách
        nghiêm ngặt — cho một agent ủy quyền, một sub-worktree được sinh ra, một plugin —
        nhưng luôn chỉ hẹp hơn. Yêu cầu bất cứ điều gì mà cha chưa bao phủ sẽ khiến TOÀN BỘ
        phép suy ra thất bại đóng, không phải một phần.
      </>
    ),
    s3Note: (
      <>
        <strong>Không tồn tại lối tắt tiện lợi kiểu "cho tôi mọi thứ".</strong>{" "}
        <code>CapabilitySet()</code> khởi tạo mặc định là rỗng; <code>grant_root()</code> là
        điểm vào DUY NHẤT được nêu tên tường minh, có thể grep được mà chính sách của host
        gọi tới. Đó không phải một comment hứa hẹn một tính chất — đó là điều mà chính cổng
        kiểm định có thể-bác-bỏ (falsifiable) của 007 §9 kiểm tra thật.
      </>
    ),
    s4Eyebrow: "sandbox/sandbox.hpp",
    s4Heading: (
      <>
        <code>SandboxProfile&lt;Strict&gt;</code> — backend mạnh nhất cho NỀN TẢNG NÀY
      </>
    ),
    s4Body: (
      <>
        Một sự cấp phát capability nói CÁI GÌ một effect có thể chạm tới; một sandbox profile
        nói về việc tiến trình chạy nó bị cách ly ĐẾN MỨC NÀO. <code>P</code> trong{" "}
        <code>SandboxProfile&lt;P&gt;</code> hoặc là một backend cụ thể, hoặc là bộ chọn{" "}
        <code>Strict</code>, được phân giải tại thời điểm build/khởi động bằng cách xếp hạng
        mọi backend thực sự hỗ trợ nền tảng hiện tại.
      </>
    ),
    strengthLabel: "độ mạnh",
    coldStartLabel: "cold start",
    s5Eyebrow: "008 §4 — Thực thi capability theo từng backend",
    s5Heading: "Cùng một bảng, nhưng hai cơ chế bên dưới hoàn toàn khác nhau",
    s5Body: (
      <>
        <code>native-jail</code> được kiểm soát bởi ACL của OS: một sự cấp phát trở thành một
        thuộc tính token và một ACL filesystem, được thiết lập một lần khi khởi chạy tiến
        trình, được kernel kiểm tra ở mỗi lần truy cập. <code>wasm</code> được kiểm soát bởi
        host function: một sự cấp phát trở thành việc linker CÓ định nghĩa một host callback
        cho một interface WIT hay KHÔNG — một capability chưa được cấp không bị từ chối tại
        thời điểm gọi, nó hoàn toàn không tồn tại như một thứ mà component có thể gọi ngay từ
        đầu. Cả hai đều là backend thật, đã kiểm thử; bảng bên dưới được đọc trực tiếp từ mã
        nguồn của chính chúng, không phải từ bản tóm tắt của RFC.
      </>
    ),
    s5TableColumns: ["Khía cạnh", "native_jail", "wasm"],
    s5Finding: (
      <>
        <strong>
          Phát hiện chính mà dòng thứ ba của bảng này dựa vào: mô hình ACL của AppContainer
          KHÔNG PHẢI là một ranh giới filesystem đủ mạnh nếu đứng một mình.
        </strong>{" "}
        Một tập hợp file host được chọn lọc — <code>win.ini</code>,{" "}
        <code>drivers\etc\hosts</code> — mang các ACE cho phép đọc dành cho{" "}
        <code>ALL APPLICATION PACKAGES</code>/<code>ALL RESTRICTED APPLICATION PACKAGES</code>{" "}
        theo mặc định của chính Windows, được kế thừa, độc lập với bất cứ điều gì backend này
        cấp hay giữ lại (ADR-004 §6, phát hiện 1). Đó chính xác là lý do vì sao 008 §1b biến
        việc trung gian hóa <code>open()</code> ở cấp trình thông dịch thành cơ chế kiểm soát
        filesystem CHÍNH và coi việc từ chối bằng ACL của kernel jail chỉ là lớp dự phòng —
        phát hiện này là trường hợp cụ thể mà cách đóng khung đó tồn tại để trả lời, không
        phải một rủi ro giả định.
      </>
    ),
    s5CompareBeforeLabel: "native_jail — khởi chạy tiến trình một lần",
    s5CompareAfterLabel: "wasm — cổng import theo từng lần load",
    s5LadderIntro: (
      <>
        Điều gì xảy ra ở mỗi lệnh gọi tool bên trong một component đã được load — thứ tự
        bind/link/instantiate/revoke thật của ADR-010 §3.2, cùng thành ngữ thu-hồi-vô-điều-kiện
        mà bước 10 của 006 §3 đã dùng cho tool gốc (native), áp dụng cho một backend hoàn toàn
        khác:
      </>
    ),
    s6Eyebrow: "008 §2a — Backend tùy chỉnh",
    s6Heading: (
      <>
        <code>SandboxBackend</code> là một concept, không phải một enum đóng gồm ba profile
      </>
    ),
    s6Body: (
      <>
        Không có gì trong bốn profile ở trên là thứ riêng của engine. Bất kỳ kiểu nào thỏa
        mãn cùng concept <code>SandboxBackend</code> — gVisor, Kata, hay một wrapper dựa trên
        microVM trong tương lai do chính người triển khai tự viết — đều gắn được vào{" "}
        <code>SandboxProfile&lt;P&gt;</code> đúng theo cách mà <code>NativeJailBackend</code>{" "}
        hay <code>WasmBackend</code> làm, bởi vì từ góc nhìn của engine không có sự khác biệt
        nào giữa chúng.
      </>
    ),
    s6Note: (
      <>
        <strong>
          Cái giá mà người triển khai phải trả khi tự cung cấp một backend: mã ở tầng tin
          cậy của host, không phải mã bị sandbox.
        </strong>{" "}
        Một backend tùy chỉnh chạy KHÔNG bị sandbox, cùng tầng tin cậy với các tool gốc
        (native) do chính bên thứ nhất viết (007 §6, T0) — engine không cố gắng cách ly chính
        cái thứ tạo ra và quản lý sự cách ly. Nó vẫn phải vượt qua đúng ngưỡng cổng thăng hạng
        §9 mà các profile tích hợp sẵn phải vượt qua (G1 tương đương về kết quả, G2 cách ly
        có positive control, G3 không có quyền hạn mặc nhiên, ...) — engine không thể tự động
        chạy cổng kiểm định đó trên mã nó không viết ra, nên đây là chuẩn mực mà một backend
        tùy chỉnh bị đánh giá theo, không phải một kiểm tra được thực thi giúp nó — đúng lập
        trường mà 007 §7 đã áp dụng cho độ tin cậy của plugin bên thứ ba.
      </>
    ),
    s7Eyebrow: "008 §4a — RemoteExecToken",
    s7Heading: (
      <>
        Một sandbox <code>remote</code> gọi ngược lại host tự xác thực bằng một token mà nó
        không thể giả mạo và host không bao giờ phải tra cứu
      </>
    ),
    s7Body: (
      <>
        Các mount và chính sách mạng của profile <code>remote</code> được hiện thực hóa thông
        qua control plane riêng của nó, ngoài băng thông — nhưng khi instance remote gọi
        NGƯỢC LẠI host (phân giải Secret, dispatch một <code>ToolCall</code>, bất kỳ effect
        nào mà nguyên tắc "egress luôn được host trung gian hóa" định tuyến qua host), host
        cần biết callback đó thuộc về <code>Exec</code> đang sống nào, mà không phải trung
        gian hóa syscall của một tiến trình nó không sở hữu. <code>RemoteExecToken</code> là
        một bearer token tự-xác-minh, kiểu macaroon — không phải một khóa tra cứu mờ vào một
        registry phía host — được xây trên đúng bộ ba nguyên thủy{" "}
        <code>mint_root</code>/<code>attenuate</code>/<code>verify</code> mà ADR-005 đã chứng
        minh cho các capability băng qua ranh giới tiến trình nói chung.
      </>
    ),
    s7StatusNote: (
      <>
        <strong>
          Trạng thái: nguyên thủy là thật và đã được chứng minh; việc đấu nối thì chưa, vì
          chưa có gì để đấu nối vào.
        </strong>{" "}
        <code>mint_root</code>/<code>attenuate</code>/<code>verify</code> trong{" "}
        <code>trust/capability_token.hpp</code> là mã thật, đã được thực thi, đã bị red-team,
        sạch ASan (ADR-005). Bản thân <code>remote</code> hiện chỉ là khung sườn (scaffolding)
        — không có backend sống nào trong codebase này gọi <code>mint_root()</code> ngày hôm
        nay. Quyết định của §4a là cơ chế nào profile <code>remote</code> sẽ dùng một khi nó
        tồn tại, dựa trên một nguyên thủy đã hoạt động thật, chứ không phải một tuyên bố rằng
        đường callback đã được đấu nối đầu-cuối.
      </>
    ),
    s8Eyebrow: "008 §5 — Tính tất định và phát lại",
    s8Heading: (
      <>
        Chỉ <code>wasm</code> là một hàm thuần túy của input của nó — các profile khác trung
        thực về việc phát lại thực sự phục vụ điều gì
      </>
    ),
    s8Body: (
      <>
        <code>Determinism</code> là hai bool trên <code>SandboxSpec</code>, không phải một
        tuyên bố mà các profile khác âm thầm kế thừa. Bật cả hai cho <code>wasm</code>, kết
        hợp với input được định địa chỉ theo nội dung (003 §3), khiến một exec trở thành một
        hàm thuần túy của <code>(component, inputs, capabilities)</code> — cache được theo
        digest, phát lại được ở chế độ offline. <code>native-jail</code> và{" "}
        <code>remote</code> thì ghi lại thay vì vậy: phát lại ở đó phục vụ lại output đã ghi,
        chứ không suy dẫn lại nó.
      </>
    ),
    s9Eyebrow: "008 §6 — Vòng đời, pooling, và trạng thái",
    s9Heading: "Ranh giới quan trọng nằm giữa các session, không phải giữa các exec trong cùng một session",
    s9Body: (
      <>
        <code>sandbox_lifetime</code> là một enum ba nhánh thật, đơn giản — không có trạng
        thái thứ tư ẩn nào. Việc tái sử dụng xuyên session bị cấm ở MỌI profile; một instance
        trong pool luôn được reset về một snapshot chụp <em>trước</em> bất kỳ input không tin
        cậy nào, không bao giờ chỉ đơn thuần là "dọn dẹp". Không điều nào trong số này chạm
        vào file: worktree (025) là trạng thái engine bền vững được mount vào sandbox, nên
        việc hủy một sandbox ở bất kỳ profile nào cũng không làm mất file nào.
      </>
    ),
    s9aEyebrow: "008 §6a — sống sót qua passivation",
    s9aHeading: "File luôn sống sót qua nó. Trạng thái trình thông dịch trong bộ nhớ thì tùy profile.",
    s9aTableColumns: ["", "wasm (plugin, 009)", "native-jail (trình thông dịch và shell, 010)"],
    s9Note: (
      <>
        <strong>Một khoảng trống được nêu tên, không phải một tính năng âm thầm hoạt động.</strong>{" "}
        Trước đây Quark từng passivate các session nhàn rỗi (ADR-028/034); ADR-037 loại bỏ
        Quark, và hiện không có cơ chế passivation session nào do host quản lý tồn tại trong{" "}
        <code>agentengine::rt::</code>. Bảng ở trên nêu hành vi MỤC TIÊU cho bất cứ khi nào
        passivation tồn tại trở lại — bản thân tính chất snapshot của <code>wasm</code> là
        thật (heap của một component chính là bộ nhớ tuyến tính của nó tại một điểm tĩnh lặng,
        nên một snapshot là một bản dump bộ nhớ cộng với trạng thái store/table/global), nhưng
        không có tương đương khả chuyển nào cho tiến trình gốc (native): CRIU chỉ chạy trên
        Linux và, theo chính tài liệu của nó, thường xuyên thất bại trên các kết nối mạng đang
        sống. Đây là một giới hạn được chấp nhận, vĩnh viễn cho trình thông dịch và shell,
        không phải một khoảng trống cần đóng lại bằng cách chọn một backend khác cho chúng —
        §1 đã loại trừ một runtime Python-trên-WASM vì lý do riêng của nó rồi.
      </>
    ),
    s10Eyebrow: "008 §7 — Thất bại và lạm dụng",
    s10Heading: (
      <>
        Một bộ hostile corpus được nêu tên, mỗi trường hợp có một positive control — và hai
        khoảng trống được nêu tên một cách trung thực
      </>
    ),
    s10Body: (
      <>
        Mỗi dòng bên dưới đều có một test trong bộ hostile suite (bị giới hạn tài nguyên,
        theo mục Machine Safety của CLAUDE.md, để việc chứng minh một fork bomb bị chặn không
        thể kéo theo cả máy dev). Hai dòng được đánh dấu là một khoảng trống đang theo dõi
        thay vì đã bị chặn — trang này nói rõ điều đó vì chính cổng thăng hạng của §9 G2 yêu
        cầu một positive control "chứng minh được là thất bại", và một tuyên bố mà trang này
        không có bằng chứng đó thì không thuộc về đây với nhãn "đã bị chặn".
      </>
    ),
    s10TableColumns: ["Trường hợp", "Cách ly / trạng thái", ""],
    containedLabel: "Đã bị chặn",
    namedGapLabel: "Khoảng trống đã nêu tên",
    s10aEyebrow: "ADR-004 §10.2 — 11 lần chạy thật, không phải một con số làm tròn",
    s10aHeading: "Giới hạn của Job Object thực sự đảm bảo điều gì, đo được — không phải giả định",
    s10aBody: (
      <>
        Một tiến trình con xoay CPU dưới <code>cpu_ms=500</code> với một backstop{" "}
        <code>wall_ms=5000</code>, chạy 11 lần qua 3 lần gọi cùng một test binary, để xem cơ
        chế nào thực sự kích hoạt.
      </>
    ),
    s10aTableColumns: ["Lần chạy", "Có kích hoạt qua JOB_OBJECT_LIMIT_JOB_TIME?", "CPU đã dùng", "Vượt mức so với 500ms"],
    s10aFiredYes: "Có",
    s10aFiredNo: "Không — backstop wall_ms kích hoạt",
    s10aNote: (
      <>
        <strong>
          3 trong 11 lần chạy tự động kích hoạt qua chính <code>JOB_OBJECT_LIMIT_JOB_TIME</code> của kernel
        </strong>
        , với mức vượt trong khoảng <strong>1.38x–8.22x</strong> ngân sách đã cấu hình và
        không có mối quan hệ rõ ràng nào giữa giới hạn đã cấu hình và mức vượt thực tế — mức
        vượt nhỏ nhất và lớn nhất cách nhau 6 lần. 8 trong 11 lần còn lại hoàn toàn không kích
        hoạt trong cửa sổ 5 giây, để cho khoảng ~9.5x–9.8x ngân sách tích lũy trước khi bộ
        giám sát <code>wall_ms</code> phía host (đã được chứng minh đáng tin cậy ở nơi khác
        với 500–504ms cho một deadline 500ms) chấm dứt job thay vào đó. Vì vậy{" "}
        <code>cpu_ms</code> được ghi nhận là best-effort, không phải một giới hạn đáng tin cậy,
        trên <code>native-jail</code> của Windows — một caller cần một giới hạn CPU/thời gian
        thật sự thì đặt <code>wall_ms</code> thành giá trị mà nó thực sự muốn được thực thi.
        Giới hạn bộ nhớ và số lượng tiến trình lại là câu chuyện ngược lại: chính xác và nhanh
        (14–22ms) và đúng chính xác (2 trong 5 lần thử spawn thành công dưới cap 3), được xác
        nhận bởi chính positive control của chúng. Đây là một khác biệt về độ tin cậy theo
        từng backend, không phải một vi phạm hợp đồng — 008 §9 G1 yêu cầu PHÂN LOẠI kết quả
        giống hệt nhau giữa các backend (một guest bị giới hạn bởi CPU luôn bị kill và luôn
        được báo cáo là resource-exceeded), không bao giờ yêu cầu độ trễ hay cơ chế thực thi
        giống hệt nhau.
      </>
    ),
    s11Eyebrow: "008 §8 — Khả năng quan sát",
    s11Heading: "Từ vựng run-event đã nêu tên một sandbox exec như một ranh giới hạng nhất",
    s11Body: (
      <>
        Trên mỗi exec, 008 §8 quy định chín trường mà một host nên có khả năng quan sát được
        — đủ để một lần hạ cấp profile hiện ra như một thay đổi trên biểu đồ metric, không
        phải một bất ngờ trong một buổi rà soát sự cố, bởi vì mọi metric đều mang theo profile.
      </>
    ),
    s11TableColumns: ["Trường", "Ý nghĩa"],
    s11Note: (
      <>
        <strong>Hình dạng đã tồn tại; bộ sinh ra nó thì chưa.</strong>{" "}
        <code>sandbox_exec_started</code>/<code>sandbox_exec_finished</code> là các thành
        viên thật của enum <code>run_event_kind</code> dùng chung DUY NHẤT mà mọi bề mặt giao
        thức (bao gồm cả AG-UI) đều dự kiến chiếu (project) từ đó — nhưng chính comment đầu
        file của <code>run_event.hpp</code> nêu rõ khoảng trống: vòng lặp lượt thật của{" "}
        <code>AgentSession</code> ngày hôm nay phát ra các sự kiện
        Run/Turn/ModelCall/StateChanged, và thực hiện đúng MỘT lệnh gọi model đồng bộ, không
        streaming, chưa bao giờ chạm tới tool pipeline — chứ đừng nói tới một sandbox exec.
        Trang này vạch rõ ranh giới đó thay vì ngụ ý rằng một luồng trực tiếp của chín trường
        trong §8 đã tồn tại ngày hôm nay.
      </>
    ),
  },
} as const;

export function ApiTrustSandboxReference() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="trust-sandbox">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {ui[lang].statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        {/* ---- 1. Declaration vs grant ----------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="capabilities" style={{ marginBottom: 22 }}>
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
            <CodePanel filename="summarize_tool.hpp">{highlightCpp(minimalCapabilitiesSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.declaresLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.declares}</p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.grantedLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.granted}</p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/06_capabilities_and_denial.cpp">
              {highlightCpp(capabilityDenialExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.outcomeNote}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="capabilities" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. The ten-step pipeline ---------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="tool-pipeline" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {toolPipelineSteps[lang].map((s) => (
                <div
                  className={`ladder-step${s.index === "4 / 7" || s.index === "5" ? " is-current" : ""}`}
                  key={s.index}
                >
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
            <div className="flow-loop-note" style={{ marginTop: 14 }}>{t.handleNote}</div>
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Attenuation only ---------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="attenuation" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="capability.hpp">{highlightCpp(attenuationExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s3Note}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Sandbox profiles ----------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="sandbox-profile" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow-row">
              {sandboxBackends[lang].map((b) => (
                <div className={`flow-node${b.strength > 0 ? " is-purple" : ""}`} key={b.name}>
                  <div className="flow-node-title">{b.name}</div>
                  <div className="flow-node-sub">
                    {t.strengthLabel} {b.strength} · {b.platforms}
                    <br />
                    {t.coldStartLabel}: {b.coldStart} · {b.note}
                  </div>
                </div>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="sandbox.hpp">{highlightCpp(sandboxProfileSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <CiteLink id="sandbox-profile" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. Backend mechanics (008 §2a/§4/§4a) ----------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="backend-mechanics" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.s5TableColumns]}
              templateColumns="0.85fr 1.7fr 1.7fr"
              rows={sandboxBackendMechanicsRows[lang].map((r) => [
                <strong key="axis">{r.axis}</strong>,
                r.nativeJail,
                r.wasm,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s5Finding}</p>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols" style={{ marginTop: 20 }}>
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.s5CompareBeforeLabel}</div>
                <CodePanel filename="native_jail_backend.cpp">
                  {highlightCpp(nativeJailProcessLaunchSnippet)}
                </CodePanel>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.s5CompareAfterLabel}</div>
                <CodePanel filename="wasm_backend.cpp">{highlightCpp(wasmImportGatingSnippet)}</CodePanel>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 20 }}>{t.s5LadderIntro}</p>
            <div className="ladder glass" style={{ padding: "6px 20px", marginTop: 12 }}>
              {capabilityEnforcementSteps[lang].map((s) => (
                <div className={`ladder-step${s.index === "5" ? " is-current" : ""}`} key={s.index}>
                  <span className="ladder-index">{s.index}</span>
                  <div>
                    <h4>{s.title}</h4>
                    <p>{s.body}</p>
                  </div>
                </div>
              ))}
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Custom backends ------------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="custom-backends" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="sandbox/sandbox.hpp">{highlightCpp(customBackendConceptSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s6Note}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 7. Remote callback authentication ---------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="remote-callback-auth" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s7Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s7Heading}</h3>
              <p>{t.s7Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {remoteCallbackAuthSteps[lang].map((s) => (
                <div className={`ladder-step${s.index === "3" ? " is-current" : ""}`} key={s.index}>
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
            <CodePanel filename="trust/capability_token.hpp">
              {highlightCpp(remoteCallbackAuthSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s7StatusNote}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 8. Determinism & replay --------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="sandbox-determinism" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s8Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s8Heading}</h3>
              <p>{t.s8Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">{sandboxDeterminismRows[lang][1].label}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  <strong style={{ color: "var(--text)" }}>{sandboxDeterminismRows[lang][1].claim}.</strong>{" "}
                  {sandboxDeterminismRows[lang][1].detail}
                </p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{sandboxDeterminismRows[lang][0].label}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  <strong style={{ color: "var(--text)" }}>{sandboxDeterminismRows[lang][0].claim}.</strong>{" "}
                  {sandboxDeterminismRows[lang][0].detail}
                </p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="sandbox.hpp">{highlightCpp(sandboxDeterminismSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <RawCite href={gh("include/agentengine/sandbox/sandbox.hpp")} label="include/agentengine/sandbox/sandbox.hpp:120-131" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 9. Lifetime, pooling, and state -------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="sandbox-lifetime" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s9Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s9Heading}</h3>
              <p>{t.s9Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {sandboxLifetimeSteps[lang].map((s) => (
                <div className={`ladder-step${s.index === "per_session" ? " is-current" : ""}`} key={s.index}>
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
            <CodePanel filename="sandbox.hpp">{highlightCpp(sandboxLifetimeSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="section-head" style={{ marginTop: 40, marginBottom: 14 }}>
              <span className="eyebrow">{t.s9aEyebrow}</span>
              <h4 style={{ fontSize: "1.05rem", margin: "6px 0" }}>{t.s9aHeading}</h4>
            </div>
            <ApiTable
              columns={[...t.s9aTableColumns]}
              templateColumns="1.4fr 1.6fr 1.8fr"
              rows={sandboxPassivationRows[lang].map((r) => [r.aspect, r.wasm, r.nativeJail])}
            />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s9Note}</p>
          </RevealItem>

          <RevealItem>
            <div style={{ marginTop: 14, display: "flex", gap: 16, flexWrap: "wrap" }}>
              <RawCite href={gh("include/agentengine/sandbox/sandbox.hpp")} label="include/agentengine/sandbox/sandbox.hpp:23,131" />
              <RawCite
                href={gh("docs/research/2026-wasm-runtime-and-state-persistence.md")}
                label="docs/research/2026-wasm-runtime-and-state-persistence.md §2"
              />
              <RawCite href={gh("decisions/ADR-037-remove-quark-as-core-runtime.md")} label="decisions/ADR-037-remove-quark-as-core-runtime.md" />
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- 10. Failure and abuse -------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="sandbox-abuse" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s10Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s10Heading}</h3>
              <p>{t.s10Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.s10TableColumns]}
              templateColumns="1.3fr 3.3fr 0.7fr"
              rows={sandboxAbuseCases[lang].map((c) => [
                c.name,
                c.containment,
                <span
                  key="status"
                  className={`status-badge status-${c.status === "contained" ? "real" : "design"}`}
                >
                  {c.status === "contained" ? t.containedLabel : t.namedGapLabel}
                </span>,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <div className="section-head" style={{ marginTop: 40, marginBottom: 14 }}>
              <span className="eyebrow">{t.s10aEyebrow}</span>
              <h4 style={{ fontSize: "1.05rem", margin: "6px 0" }}>{t.s10aHeading}</h4>
              <p>{t.s10aBody}</p>
            </div>
            <ApiTable
              columns={[...t.s10aTableColumns]}
              templateColumns="0.6fr 2.1fr 1.1fr 1.1fr"
              rows={sandboxJobTimeRuns.map((r) => [
                String(r.run),
                r.fired ? t.s10aFiredYes : t.s10aFiredNo,
                `${r.cpuConsumedMs} ms`,
                r.overrun,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s10aNote}</p>
          </RevealItem>

          <RevealItem>
            <div style={{ marginTop: 14, display: "flex", gap: 16, flexWrap: "wrap" }}>
              <RawCite
                href={gh("decisions/ADR-004-appcontainer-native-jail-windows-backend.md")}
                label="decisions/ADR-004-appcontainer-native-jail-windows-backend.md §10"
              />
              <RawCite href={gh("tests/test_native_jail_abuse_corpus_windows.cpp")} label="tests/test_native_jail_abuse_corpus_windows.cpp" />
              <RawCite href={gh("tests/test_native_jail_abuse_corpus_linux.cpp")} label="tests/test_native_jail_abuse_corpus_linux.cpp" />
              <RawCite
                href={gh("tests/test_mediated_python_runner_hostile_corpus.cpp")}
                label="tests/test_mediated_python_runner_hostile_corpus.cpp"
              />
              <RawCite
                href={gh("tests/test_mediated_shell_runner_hostile_corpus.cpp")}
                label="tests/test_mediated_shell_runner_hostile_corpus.cpp"
              />
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- 11. Observability --------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="sandbox-observability" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s11Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s11Heading}</h3>
              <p>{t.s11Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.s11TableColumns]}
              templateColumns="1.4fr 3fr"
              rows={sandboxObservabilityFields[lang].map((f) => [<code key="n">{f.name}</code>, f.meaning])}
            />
          </RevealItem>

          <RevealItem>
            <CodePanel filename="core/run_event.hpp">{highlightCpp(sandboxRunEventSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s11Note}</p>
          </RevealItem>

          <RevealItem>
            <RawCite href={gh("include/agentengine/core/run_event.hpp")} label="include/agentengine/core/run_event.hpp" />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
