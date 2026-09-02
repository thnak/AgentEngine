import {
  attenuationExampleSnippet,
  capabilityDenialErrorSnippet,
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
  secretQuarantineExampleSnippet,
  secretQuarantineOwnership,
  toolPipelineSteps,
  trustEntries,
  wasmImportGatingSnippet,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
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
        Most tools touch no privileged effect at all, and declare nothing here.{" "}
        <code>declared_capabilities()</code> already returns an empty ceiling unless overridden.
        Add exactly the <code>cap::decl::*</code> tag matching the one effect a tool actually
        reaches — a single <code>FsWrite&lt;"work"&gt;</code>, say — never a broader set for
        "in case it's needed later":
      </>
    ),
    s1RecoNote: <>007 §3 — the empty-by-default capability declaration rule</>,
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
        <code>CapabilitySet::grant_root(...)</code> is called by the host, never reachable
        from anything derived from model output. This is the only thing step 4/7 of the
        pipeline actually checks against.
      </>
    ),
    grantedNote: <>I3 — model output is data, never authority</>,
    outcomeNote: (
      <>
        <strong>The same tool call produces two different outcomes.</strong> The only variable
        across the two sessions above is what <code>CapabilitySet</code> was handed to{" "}
        <code>set_capabilities()</code>. Denial is an ordinary tool error fed back to the model,
        not a run-level failure — the run still converges; it just never invokes{" "}
        <code>write_note</code>'s real body.
      </>
    ),
    denialErrorNote: (
      <>
        <strong>What that tool error actually contains:</strong> <code>audit.error_code</code>{" "}
        is always the stable <code>tool.capability_not_held</code> code, regardless of which
        capability kind was missing. The <code>Error</code> content's <code>message</code> stays
        deliberately silent about which capability was checked or held — proven by asserting
        the message does not contain the capability's own name, not merely by reading a comment
        that promises it. A caller cannot fingerprint a session's granted capabilities by
        triggering denials and inspecting the wording.
      </>
    ),
    s2Eyebrow: "tool_pipeline.hpp — invoke_tool()",
    s2Heading: "Every tool call, real or model-requested, crosses the same ten steps",
    s2Body: (
      <>
        <code>invoke_tool()</code> is the one function every native tool call goes through,
        whether it came from the model or a workflow node. Two of these ten steps are the
        actual enforcement gates; the rest are resolution, bookkeeping, or a documented no-op.
      </>
    ),
    s2Note: <>006 §3 — the ten-step invoke_tool() pipeline</>,
    handleNote: (
      <>
        A bound capability handle from step 4/7 is valid for exactly one call. Step 10 revokes
        it unconditionally — success or failure — before the result is even normalized, so a
        handle from call N cannot be reused at call N+1. A regression test proves that, not
        just a comment.
      </>
    ),
    s3Eyebrow: "CapabilitySet::attenuate()",
    s3Heading: "Deriving a narrower set is the only direction that compiles",
    s3Body: (
      <>
        A parent <code>CapabilitySet</code> can hand out a strictly narrower child — a
        delegate agent, a spawned sub-worktree, a plugin — but only ever narrower. Asking for
        anything the parent doesn't already cover fails the whole derivation closed, not
        partially.
      </>
    ),
    s3Note: (
      <>
        <strong>No convenience "give me everything" shortcut exists.</strong>{" "}
        <code>CapabilitySet()</code> default-constructs empty. <code>grant_root()</code> is the
        one explicitly-named, greppable entry point host policy calls — not a comment promising
        a property, but the thing a falsifiable test gate actually checks.
      </>
    ),
    s3NoteCite: <>007 §9 — the capability-narrowing test gate</>,
    s3bEyebrow: "ADR-068 — trust/secret_quarantine.hpp",
    s3bHeading: <>Quarantining a secret that shows up where nobody declared one</>,
    s3bBody: (
      <>
        <code>trust/secret.hpp</code> already gives an operator-declared secret a
        capability-gated, zeroizing, declare-then-resolve path. A secret that shows up
        incidentally — a pasted API key, a tool result echoing a leaked credential — was never
        declared anywhere, so nothing catches it. <code>QuarantineSecretStore</code> adds a
        second, mint-at-runtime path that satisfies the same <code>SecretStore</code> concept.
        It content-addresses each secret by a MAC of the detected bytes, reusing{" "}
        <code>hmac_sha256</code>, the one audited digest primitive already in this codebase.
        The same value quarantined by two independent callers collapses onto one{" "}
        <code>SecretRef</code>, with no coordination between them. The shape follows HashiCorp
        Vault's tokenization model: an opaque <code>quarantine:&lt;hex&gt;</code> reference,
        nothing recoverable without a <code>resolve()</code> round trip — not a
        reversible-ciphertext-in-place design.
      </>
    ),
    s3bHmacNote: <>ADR-021 — hmac_sha256 as the audited digest primitive</>,
    s3bOwnershipIntro: "AgentEngine ships no detection of its own — ADR-068 §2 owns the seam and a narrow structural guarantee on the audit event; detection and audit durability are the host's:",
    s3bOwnershipColumns: ["Concern", "Owner", "Notes"],
    s3bTriggerIntro: "The one thing quarantine() itself decides is bookkeeping, not trust: HOW a piece of text got here changes what a host is later allowed to do with the resulting ref — the ref-minting call never grants anything by itself, either way.",
    s3bVerifiedLabel: "verified_user_content",
    s3bVerified: (
      <>
        The engine itself verified this content's provenance — a <code>Message</code> whose{" "}
        <code>content_origin</code> is genuinely <code>user</code> — before{" "}
        <code>scan_and_quarantine()</code> ever ran. <code>grant_eligible_ref_names()</code>{" "}
        names the resulting ref, so a host MAY fold it into a real grant at its own next{" "}
        <code>CapabilitySet::grant_root()</code> call.
      </>
    ),
    s3bAgentLabel: "agent_initiated",
    s3bAgent: (
      <>
        A model-issued <code>quarantine_secret</code> tool call supplied this text as an
        argument — model output, not an engine-verified fact. Never grant-eligible,
        structurally: there is no branch of <code>grant_eligible_ref_names()</code> that can
        return a ref minted this way, no matter what the model passes.
      </>
    ),
    s3bNote: (
      <>
        <strong>A fatal finding this mechanism had to close on its own.</strong> The design
        originally drafted <code>quarantine()</code> to mint a ref and grant{" "}
        <code>cap::Secret</code> in the same step. That doesn't compile against the real{" "}
        <code>CapabilitySet</code>: it's empty by construction, has no method to add one
        capability incrementally, and only <code>grant_root()</code> — which replaces the whole
        set — is reachable from host code, never from anything derived from model output. A
        sharper problem sat underneath even a fixed API: <code>QuarantineSecretTool::invoke</code>
        's argument is model-supplied, so an auto-grant on that path would let a manipulated
        model mint itself a resolvable secret reference for text that was never actually the
        user's own value. That's model output becoming authority, exactly what I3 forbids. The
        fix splits the concern: quarantining never mutates a capability set, and
        grant-eligibility is a host-only query, reachable from no <code>Tool::invoke()</code>{" "}
        body, on a boundary that was already safe under I3.
      </>
    ),
    s3bResidualNote: (
      <>
        <strong>A named limitation, not a hidden one.</strong> A deployment that wires neither a{" "}
        <code>SecretDetector</code> nor a <code>QuarantineAuditHook</code> gets no protection
        and no audit trail — fails open, not closed, on missing host configuration. Whether
        that default is right for a given deployment is a decision ADR-068 leaves to the host,
        not one it makes silently.
      </>
    ),
    s4Eyebrow: "sandbox/sandbox.hpp",
    s4Heading: (
      <>
        <code>SandboxProfile&lt;Strict&gt;</code> — the strongest backend for this platform
      </>
    ),
    s4Body: (
      <>
        A capability grant controls what an effect may reach. A sandbox profile controls how
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
        filesystem ACL, set up once at process launch and checked by the kernel on every
        access. <code>wasm</code> is host-function-gated: a grant becomes whether the linker
        defines a host callback for a WIT interface at all. An ungranted capability isn't
        denied at call time — it never exists as something the component could call in the
        first place. The row below is read directly off each backend's own source, not the
        RFC's summary of it.
      </>
    ),
    s5TableColumns: ["Axis", "native_jail", "wasm"],
    s5Finding: (
      <>
        <strong>
          The headline finding this table's third row is built on: AppContainer's ACL model is
          not a sufficient filesystem boundary by itself.
        </strong>{" "}
        A curated set of host files — <code>win.ini</code>,{" "}
        <code>drivers\etc\hosts</code> — carry{" "}
        <code>ALL APPLICATION PACKAGES</code>/<code>ALL RESTRICTED APPLICATION PACKAGES</code>{" "}
        read ACEs by Windows' own default, inherited, independent of whatever this backend
        grants or withholds. That is exactly why 008 §1b makes interpreter-level{" "}
        <code>open()</code> mediation the primary filesystem control and treats the kernel
        jail's ACL denial as backstop only. This finding is the concrete case that framing
        exists to answer, not a hypothetical risk.
      </>
    ),
    s5FindingNote: <>ADR-004 §6, finding 1</>,
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
        A custom backend runs unsandboxed, at the same trust tier as first-party native tools —
        the engine does not attempt to contain the thing that creates and manages containment.
        It must still clear the same promotion-gate bar the built-in profiles do: outcome
        parity, containment backed by a positive control, no ambient authority. The engine
        cannot run that gate automatically on code it did not write, so this is the standard a
        custom backend is held to, not a check enforced for it — the same posture the engine
        already takes toward third-party plugin trust.
      </>
    ),
    s6NoteCite: <>007 §6 (T0) · 007 §9 (G1–G3) · 007 §7 (third-party plugin trust)</>,
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
        its own control plane, out of band. When the remote instance calls back to the host —
        for secret resolution, a <code>ToolCall</code> dispatch, or anything the "egress is
        always host-mediated" rule routes through the host — the host needs to know which live{" "}
        <code>Exec</code> that callback belongs to, without syscall-mediating a process it
        doesn't own. <code>RemoteExecToken</code> is a self-verifying, macaroon-style bearer
        token, not an opaque lookup key into a host-side registry. It's built on the same{" "}
        <code>mint_root</code>/<code>attenuate</code>/<code>verify</code> primitive proved out
        for capabilities crossing a process boundary generally.
      </>
    ),
    s7Note: <>ADR-005 — the mint_root/attenuate/verify capability-token primitive</>,
    s7StatusNote: (
      <>
        <strong>
          Status: the primitive is real and proven; the wiring isn't, because there's nothing
          to wire it to yet.
        </strong>{" "}
        <code>mint_root</code>/<code>attenuate</code>/<code>verify</code> in{" "}
        <code>trust/capability_token.hpp</code> are real, executed, red-teamed, ASan-clean
        code. <code>remote</code> itself is scaffolding only — no live backend in this codebase
        calls <code>mint_root()</code> today. §4a's decision is which mechanism the{" "}
        <code>remote</code> profile will use once it exists, grounded in a primitive that
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
        content-addressed inputs, makes one exec a pure function of{" "}
        <code>(component, inputs, capabilities)</code> — cacheable by digest and replayable
        offline. <code>native-jail</code> and <code>remote</code> record instead: replay
        there serves the recorded output; it does not re-derive it.
      </>
    ),
    s8Note: <>003 §3 — content-addressed inputs</>,
    s9Eyebrow: "008 §6 — Lifetime, pooling, and state",
    s9Heading: "The boundary that matters is between sessions, not between execs within one",
    s9Body: (
      <>
        <code>sandbox_lifetime</code> is a plain three-way enum — no hidden fourth state.
        Cross-session reuse is prohibited in every profile; a pooled instance is always reset
        to a snapshot taken <em>before</em> any untrusted input, never merely "cleaned". None
        of this touches files: the worktree is durable engine state mounted into the sandbox,
        so destroying a sandbox on any profile loses no files.
      </>
    ),
    s9BodyNote: <>025 — the worktree as durable, sandbox-mounted engine state</>,
    s9aEyebrow: "008 §6a — surviving passivation",
    s9aHeading: "Files always survive it. In-memory interpreter state depends on the profile.",
    s9aTableColumns: ["", "wasm (plugins, 009)", "native-jail (interpreter and shell, 010)"],
    s9Note: (
      <>
        <strong>A named gap, not a silently-working feature.</strong> Quark previously
        passivated idle sessions; ADR-037 removed Quark, and no host-managed
        session-passivation mechanism currently exists in <code>agentengine::rt::</code>. The
        table above states the target behavior for whenever passivation exists again. The{" "}
        <code>wasm</code> snapshot property itself is real: a component's heap is its linear
        memory at a quiescent point, so a snapshot is a memory dump plus store/table/global
        state. There is no portable equivalent for native processes — CRIU is Linux-only and,
        by its own docs, frequently fails on live network connections. This is an accepted,
        permanent limitation for the interpreter and shell, not a gap to close by picking a
        different backend for them; §1 already rules out a WASM Python runtime for its own
        reasons.
      </>
    ),
    s9NoteCite: <>ADR-028/034 — Quark's prior session passivation</>,
    s10Eyebrow: "008 §7 — Failure and abuse",
    s10Heading: (
      <>
        A named hostile corpus, each case with a positive control — and two honestly-named
        gaps
      </>
    ),
    s10Body: (
      <>
        Every row below has a test in the hostile suite. The suite is resource-capped, per
        CLAUDE.md's Machine Safety rule, so proving a fork bomb is contained can't take the
        dev box down with it. Two rows are marked as a tracked gap rather than contained: this
        page says so because the promotion gate requires a positive control that
        "demonstrably fails", and a claim this page can't back with one doesn't belong here as
        "contained".
      </>
    ),
    s10Note: <>008 §9 G2 — the positive-control requirement for containment claims</>,
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
        contract violation. 008 §9 G1 requires identical outcome classification across
        backends — a CPU-bound guest is always killed and always reported as
        resource-exceeded — never identical enforcement latency or mechanism.
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
        of the one shared <code>run_event_kind</code> enum that every protocol surface,
        AG-UI included, is meant to project from. <code>run_event.hpp</code>'s own top comment
        names the gap plainly: <code>AgentSession</code>'s real turn loop fires
        Run/Turn/ModelCall/StateChanged events today, and makes exactly one synchronous,
        non-streaming model call that never reaches the tool pipeline, let alone a sandbox
        exec. This page draws that line rather than implying a live stream of §8's nine fields
        exists today.
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
        Hầu hết tool không chạm tới effect đặc quyền nào cả, và không khai báo gì ở đây.{" "}
        <code>declared_capabilities()</code> đã trả về một ceiling rỗng trừ khi được ghi đè.
        Chỉ thêm đúng thẻ <code>cap::decl::*</code> khớp với effect duy nhất mà tool thực sự
        chạm tới — ví dụ một <code>FsWrite&lt;"work"&gt;</code> duy nhất — không bao giờ khai
        báo rộng hơn "phòng khi sau này cần":
      </>
    ),
    s1RecoNote: <>007 §3 — quy tắc mặc định-rỗng cho khai báo capability</>,
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
        <code>CapabilitySet::grant_root(...)</code> do host gọi, không bao giờ chạm tới được
        từ bất cứ thứ gì bắt nguồn từ đầu ra của model. Đây là thứ duy nhất mà bước 4/7 của
        pipeline thực sự kiểm tra dựa vào.
      </>
    ),
    grantedNote: <>I3 — đầu ra của model là dữ liệu, không bao giờ là authority</>,
    outcomeNote: (
      <>
        <strong>Cùng một lệnh gọi tool, hai kết quả khác nhau.</strong> Biến số duy nhất giữa
        hai session ở trên là <code>CapabilitySet</code> nào được trao cho{" "}
        <code>set_capabilities()</code>. Từ chối là một lỗi tool bình thường được đưa trở lại
        cho model, không phải một thất bại ở cấp run — run vẫn hội tụ, chỉ là nó không bao giờ
        gọi thực thi phần thân thật của <code>write_note</code>.
      </>
    ),
    denialErrorNote: (
      <>
        <strong>Lỗi tool đó thực sự chứa gì:</strong> <code>audit.error_code</code> luôn là mã
        ổn định <code>tool.capability_not_held</code>, bất kể loại capability nào bị thiếu.{" "}
        <code>message</code> của nội dung <code>Error</code> cố tình im lặng về việc capability
        nào đã được kiểm tra hay đang được nắm giữ — được chứng minh bằng cách khẳng định
        message không chứa tên riêng của capability đó, không chỉ bằng cách đọc một comment
        hứa hẹn điều đó. Một caller không thể dò ra tập capability mà một session được cấp
        bằng cách kích hoạt các lần từ chối rồi soi câu chữ.
      </>
    ),
    s2Eyebrow: "tool_pipeline.hpp — invoke_tool()",
    s2Heading: "Mọi lệnh gọi tool, dù thật hay do model yêu cầu, đều đi qua đúng mười bước",
    s2Body: (
      <>
        <code>invoke_tool()</code> là hàm duy nhất mà mọi lệnh gọi tool gốc (native) đi qua,
        bất kể nó đến từ model hay từ một node workflow. Hai trong số mười bước này là các
        cổng thực thi thật; phần còn lại là phân giải, sổ sách (bookkeeping), hoặc một no-op
        đã được ghi nhận.
      </>
    ),
    s2Note: <>006 §3 — pipeline invoke_tool() mười bước</>,
    handleNote: (
      <>
        Một capability handle đã ràng buộc từ bước 4/7 chỉ có hiệu lực cho đúng một lệnh gọi.
        Bước 10 thu hồi nó vô điều kiện — dù thành công hay thất bại — trước cả khi kết quả
        được chuẩn hóa, nên một handle từ lệnh gọi N không thể tái sử dụng ở lệnh gọi N+1. Một
        test hồi quy chứng minh điều đó, không chỉ một comment.
      </>
    ),
    s3Eyebrow: "CapabilitySet::attenuate()",
    s3Heading: "Suy ra một tập hẹp hơn là hướng duy nhất biên dịch được",
    s3Body: (
      <>
        Một <code>CapabilitySet</code> cha có thể trao ra một tập con hẹp hơn một cách
        nghiêm ngặt — cho một agent ủy quyền, một sub-worktree được sinh ra, một plugin —
        nhưng luôn chỉ hẹp hơn. Yêu cầu bất cứ điều gì mà cha chưa bao phủ sẽ khiến toàn bộ
        phép suy ra thất bại đóng, không phải một phần.
      </>
    ),
    s3Note: (
      <>
        <strong>Không tồn tại lối tắt tiện lợi kiểu "cho tôi mọi thứ".</strong>{" "}
        <code>CapabilitySet()</code> khởi tạo mặc định là rỗng. <code>grant_root()</code> là
        điểm vào duy nhất được nêu tên tường minh, có thể grep được, mà chính sách của host
        gọi tới — không phải một comment hứa hẹn một tính chất, mà là điều một cổng kiểm định
        có thể-bác-bỏ (falsifiable) thực sự kiểm tra.
      </>
    ),
    s3NoteCite: <>007 §9 — cổng kiểm định thu hẹp capability</>,
    s3bEyebrow: "ADR-068 — trust/secret_quarantine.hpp",
    s3bHeading: <>Cách ly (quarantine) một secret xuất hiện ở nơi không ai khai báo nó</>,
    s3bBody: (
      <>
        <code>trust/secret.hpp</code> đã cho một secret được operator khai báo một đường đi
        capability-gated, tự xóa (zeroizing), theo kiểu declare-rồi-resolve. Một secret xuất
        hiện một cách tình cờ — một API key bị dán nhầm vào, một kết quả tool lặp lại một
        credential bị lộ — thì chưa từng được khai báo ở đâu cả, nên không có gì bắt được nó.{" "}
        <code>QuarantineSecretStore</code> thêm một đường đi thứ hai, tạo secret ngay tại
        runtime, thỏa cùng khái niệm <code>SecretStore</code>. Nó định địa chỉ mỗi secret theo
        nội dung bằng một MAC trên các byte phát hiện được, tái sử dụng{" "}
        <code>hmac_sha256</code>, primitive digest duy nhất đã được kiểm toán trong codebase
        này. Cùng một giá trị được hai caller độc lập quarantine sẽ gộp về đúng một{" "}
        <code>SecretRef</code>, mà không cần hai bên phối hợp với nhau. Hình dạng này theo
        đúng kiểu tokenization của HashiCorp Vault: một tham chiếu{" "}
        <code>quarantine:&lt;hex&gt;</code> mờ đục, không có gì khôi phục được nếu không quay
        lại qua <code>resolve()</code> — không phải kiểu ciphertext-có-thể-đảo-ngược-tại-chỗ.
      </>
    ),
    s3bHmacNote: <>ADR-021 — hmac_sha256 là primitive digest đã được kiểm toán</>,
    s3bOwnershipIntro: "AgentEngine không đóng gói bất kỳ cơ chế phát hiện nào của riêng mình — ADR-068 §2 sở hữu ranh giới (seam) và một đảm bảo cấu trúc hẹp trên kiểu audit event; phát hiện và độ bền của audit là việc của host:",
    s3bOwnershipColumns: ["Mối quan tâm", "Ai sở hữu", "Ghi chú"],
    s3bTriggerIntro: "Điều duy nhất mà chính quarantine() quyết định là bookkeeping, không phải trust: đoạn text này đến bằng cách nào sẽ quyết định host được phép làm gì với ref kết quả về sau — bản thân lệnh gọi tạo ref không bao giờ cấp phát bất cứ điều gì, dù theo cách nào.",
    s3bVerifiedLabel: "verified_user_content",
    s3bVerified: (
      <>
        Chính engine đã xác minh nguồn gốc của nội dung này — một <code>Message</code> có{" "}
        <code>content_origin</code> thực sự là <code>user</code> — trước khi{" "}
        <code>scan_and_quarantine()</code> từng chạy. <code>grant_eligible_ref_names()</code>{" "}
        nêu tên ref kết quả, để một host có thể gộp nó vào một lần cấp phát thật ở lần gọi{" "}
        <code>CapabilitySet::grant_root()</code> kế tiếp của chính mình.
      </>
    ),
    s3bAgentLabel: "agent_initiated",
    s3bAgent: (
      <>
        Một lệnh gọi tool <code>quarantine_secret</code> do model phát ra đã cung cấp đoạn
        text này như một argument — đây là đầu ra của model, không phải một sự thật đã được
        engine xác minh. Không bao giờ đủ điều kiện cấp phát, về mặt cấu trúc: không có nhánh
        nào của <code>grant_eligible_ref_names()</code> có thể trả về một ref được tạo theo
        cách này, bất kể model truyền vào gì.
      </>
    ),
    s3bNote: (
      <>
        <strong>Một phát hiện chí mạng mà cơ chế này phải tự đóng lại.</strong> Thiết kế ban
        đầu phác thảo <code>quarantine()</code> vừa tạo ref vừa cấp <code>cap::Secret</code>{" "}
        trong cùng một bước. Điều đó không biên dịch được với <code>CapabilitySet</code> thật:
        nó rỗng theo cấu trúc, không có phương thức nào để thêm từng capability một, và chỉ{" "}
        <code>grant_root()</code> — thay thế toàn bộ tập hợp — mới chạm tới được từ mã của
        host, không bao giờ từ bất cứ thứ gì bắt nguồn từ đầu ra của model. Một vấn đề sắc hơn
        nằm ngay dưới cả một API đã sửa: argument của <code>QuarantineSecretTool::invoke</code>{" "}
        là do model cung cấp, nên nếu đường đó tự động cấp phát, một model bị thao túng có thể
        tự tạo cho mình một tham chiếu secret resolve được cho một đoạn text chưa từng thực sự
        là giá trị của người dùng. Đó là đầu ra của model trở thành authority, đúng điều I3
        cấm. Cách sửa tách mối quan tâm ra: quarantine không bao giờ thay đổi một capability
        set, và tính đủ điều kiện cấp phát là một truy vấn chỉ dành cho host, không có đường
        nào từ thân hàm <code>Tool::invoke()</code> chạm tới được, trên một ranh giới vốn đã
        an toàn theo I3 từ trước.
      </>
    ),
    s3bResidualNote: (
      <>
        <strong>Một giới hạn được nêu tên, không phải bị giấu đi.</strong> Một triển khai
        không đấu nối cả <code>SecretDetector</code> lẫn <code>QuarantineAuditHook</code> sẽ
        không có bảo vệ nào và không có audit trail nào — an toàn kiểu "mở" (fail open) chứ
        không phải "đóng" khi thiếu cấu hình từ host. Mặc định đó có phù hợp với một triển khai
        cụ thể hay không là quyết định ADR-068 để ngỏ cho host, không tự ý quyết định trong im
        lặng.
      </>
    ),
    s4Eyebrow: "sandbox/sandbox.hpp",
    s4Heading: (
      <>
        <code>SandboxProfile&lt;Strict&gt;</code> — backend mạnh nhất cho nền tảng này
      </>
    ),
    s4Body: (
      <>
        Một sự cấp phát capability kiểm soát effect nào có thể chạm tới. Một sandbox profile
        kiểm soát mức độ cách ly của tiến trình chạy nó. <code>P</code> trong{" "}
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
        trình và được kernel kiểm tra ở mỗi lần truy cập. <code>wasm</code> được kiểm soát bởi
        host function: một sự cấp phát trở thành việc linker có định nghĩa một host callback
        cho một interface WIT hay không. Một capability chưa được cấp không bị từ chối tại
        thời điểm gọi — nó hoàn toàn không tồn tại như một thứ mà component có thể gọi ngay từ
        đầu. Bảng bên dưới được đọc trực tiếp từ mã nguồn của chính từng backend, không phải
        từ bản tóm tắt của RFC.
      </>
    ),
    s5TableColumns: ["Khía cạnh", "native_jail", "wasm"],
    s5Finding: (
      <>
        <strong>
          Phát hiện chính mà dòng thứ ba của bảng này dựa vào: mô hình ACL của AppContainer
          không phải là một ranh giới filesystem đủ mạnh nếu đứng một mình.
        </strong>{" "}
        Một tập hợp file host được chọn lọc — <code>win.ini</code>,{" "}
        <code>drivers\etc\hosts</code> — mang các ACE cho phép đọc dành cho{" "}
        <code>ALL APPLICATION PACKAGES</code>/<code>ALL RESTRICTED APPLICATION PACKAGES</code>{" "}
        theo mặc định của chính Windows, được kế thừa, độc lập với bất cứ điều gì backend này
        cấp hay giữ lại. Đó chính xác là lý do vì sao 008 §1b biến việc trung gian hóa{" "}
        <code>open()</code> ở cấp trình thông dịch thành cơ chế kiểm soát filesystem chính và
        coi việc từ chối bằng ACL của kernel jail chỉ là lớp dự phòng. Phát hiện này là trường
        hợp cụ thể mà cách đóng khung đó tồn tại để trả lời, không phải một rủi ro giả định.
      </>
    ),
    s5FindingNote: <>ADR-004 §6, phát hiện 1</>,
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
        Một backend tùy chỉnh chạy không bị sandbox, cùng tầng tin cậy với các tool gốc
        (native) do chính bên thứ nhất viết — engine không cố gắng cách ly chính cái thứ tạo
        ra và quản lý sự cách ly. Nó vẫn phải vượt qua đúng ngưỡng cổng thăng hạng mà các
        profile tích hợp sẵn phải vượt qua: tương đương về kết quả, cách ly có positive
        control, không có quyền hạn mặc nhiên. Engine không thể tự động chạy cổng kiểm định đó
        trên mã nó không viết ra, nên đây là chuẩn mực mà một backend tùy chỉnh bị đánh giá
        theo, không phải một kiểm tra được thực thi giúp nó — đúng lập trường mà engine đã áp
        dụng cho độ tin cậy của plugin bên thứ ba.
      </>
    ),
    s6NoteCite: <>007 §6 (tầng tin cậy T0) · 007 §9 (các cổng G1–G3) · 007 §7 (độ tin cậy plugin bên thứ ba)</>,
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
        qua control plane riêng của nó, ngoài băng thông. Khi instance remote gọi ngược lại
        host — để phân giải Secret, dispatch một <code>ToolCall</code>, hoặc bất kỳ effect nào
        mà nguyên tắc "egress luôn được host trung gian hóa" định tuyến qua host — host cần
        biết callback đó thuộc về <code>Exec</code> đang sống nào, mà không phải trung gian
        hóa syscall của một tiến trình nó không sở hữu. <code>RemoteExecToken</code> là một
        bearer token tự-xác-minh, kiểu macaroon, không phải một khóa tra cứu mờ vào một
        registry phía host. Nó được xây trên đúng bộ ba nguyên thủy{" "}
        <code>mint_root</code>/<code>attenuate</code>/<code>verify</code> đã được chứng minh
        cho các capability băng qua ranh giới tiến trình nói chung.
      </>
    ),
    s7Note: <>ADR-005 — nguyên thủy capability-token mint_root/attenuate/verify</>,
    s7StatusNote: (
      <>
        <strong>
          Trạng thái: nguyên thủy là thật và đã được chứng minh; việc đấu nối thì chưa, vì
          chưa có gì để đấu nối vào.
        </strong>{" "}
        <code>mint_root</code>/<code>attenuate</code>/<code>verify</code> trong{" "}
        <code>trust/capability_token.hpp</code> là mã thật, đã được thực thi, đã bị red-team,
        sạch ASan. Bản thân <code>remote</code> hiện chỉ là khung sườn (scaffolding) — không
        có backend sống nào trong codebase này gọi <code>mint_root()</code> ngày hôm nay.
        Quyết định của §4a là cơ chế nào profile <code>remote</code> sẽ dùng một khi nó tồn
        tại, dựa trên một nguyên thủy đã hoạt động thật, chứ không phải một tuyên bố rằng
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
        hợp với input được định địa chỉ theo nội dung, khiến một exec trở thành một hàm thuần
        túy của <code>(component, inputs, capabilities)</code> — cache được theo digest và
        phát lại được ở chế độ offline. <code>native-jail</code> và <code>remote</code> thì
        ghi lại thay vì vậy: phát lại ở đó phục vụ lại output đã ghi, chứ không suy dẫn lại nó.
      </>
    ),
    s8Note: <>003 §3 — input được định địa chỉ theo nội dung</>,
    s9Eyebrow: "008 §6 — Vòng đời, pooling, và trạng thái",
    s9Heading: "Ranh giới quan trọng nằm giữa các session, không phải giữa các exec trong cùng một session",
    s9Body: (
      <>
        <code>sandbox_lifetime</code> là một enum ba nhánh đơn giản — không có trạng thái thứ
        tư ẩn nào. Việc tái sử dụng xuyên session bị cấm ở mọi profile; một instance trong pool
        luôn được reset về một snapshot chụp <em>trước</em> bất kỳ input không tin cậy nào,
        không bao giờ chỉ đơn thuần là "dọn dẹp". Không điều nào trong số này chạm vào file:
        worktree là trạng thái engine bền vững được mount vào sandbox, nên việc hủy một sandbox
        ở bất kỳ profile nào cũng không làm mất file nào.
      </>
    ),
    s9BodyNote: <>025 — worktree như trạng thái engine bền vững, được mount vào sandbox</>,
    s9aEyebrow: "008 §6a — sống sót qua passivation",
    s9aHeading: "File luôn sống sót qua nó. Trạng thái trình thông dịch trong bộ nhớ thì tùy profile.",
    s9aTableColumns: ["", "wasm (plugin, 009)", "native-jail (trình thông dịch và shell, 010)"],
    s9Note: (
      <>
        <strong>Một khoảng trống được nêu tên, không phải một tính năng âm thầm hoạt động.</strong>{" "}
        Trước đây Quark từng passivate các session nhàn rỗi; ADR-037 loại bỏ Quark, và hiện
        không có cơ chế passivation session nào do host quản lý tồn tại trong{" "}
        <code>agentengine::rt::</code>. Bảng ở trên nêu hành vi mục tiêu cho bất cứ khi nào
        passivation tồn tại trở lại. Bản thân tính chất snapshot của <code>wasm</code> là
        thật: heap của một component chính là bộ nhớ tuyến tính của nó tại một điểm tĩnh lặng,
        nên một snapshot là một bản dump bộ nhớ cộng với trạng thái store/table/global. Không
        có tương đương khả chuyển nào cho tiến trình gốc (native) — CRIU chỉ chạy trên Linux
        và, theo chính tài liệu của nó, thường xuyên thất bại trên các kết nối mạng đang sống.
        Đây là một giới hạn được chấp nhận, vĩnh viễn cho trình thông dịch và shell, không phải
        một khoảng trống cần đóng lại bằng cách chọn một backend khác cho chúng; §1 đã loại
        trừ một runtime Python-trên-WASM vì lý do riêng của nó rồi.
      </>
    ),
    s9NoteCite: <>ADR-028/034 — cơ chế passivation session trước đây của Quark</>,
    s10Eyebrow: "008 §7 — Thất bại và lạm dụng",
    s10Heading: (
      <>
        Một bộ hostile corpus được nêu tên, mỗi trường hợp có một positive control — và hai
        khoảng trống được nêu tên một cách trung thực
      </>
    ),
    s10Body: (
      <>
        Mỗi dòng bên dưới đều có một test trong bộ hostile suite. Bộ suite này bị giới hạn tài
        nguyên, theo quy tắc Machine Safety của CLAUDE.md, để việc chứng minh một fork bomb bị
        chặn không thể kéo theo cả máy dev. Hai dòng được đánh dấu là một khoảng trống đang
        theo dõi thay vì đã bị chặn: trang này nói rõ điều đó vì cổng thăng hạng yêu cầu một
        positive control "chứng minh được là thất bại", và một tuyên bố mà trang này không có
        bằng chứng đó thì không thuộc về đây với nhãn "đã bị chặn".
      </>
    ),
    s10Note: <>008 §9 G2 — yêu cầu positive control cho tuyên bố containment</>,
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
        từng backend, không phải một vi phạm hợp đồng. 008 §9 G1 yêu cầu phân loại kết quả
        giống hệt nhau giữa các backend — một guest bị giới hạn bởi CPU luôn bị kill và luôn
        được báo cáo là resource-exceeded — không bao giờ yêu cầu độ trễ hay cơ chế thực thi
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
        viên thật của enum <code>run_event_kind</code> dùng chung duy nhất mà mọi bề mặt giao
        thức, kể cả AG-UI, đều dự kiến chiếu (project) từ đó. Chính comment đầu file của{" "}
        <code>run_event.hpp</code> nêu rõ khoảng trống: vòng lặp lượt thật của{" "}
        <code>AgentSession</code> ngày hôm nay phát ra các sự kiện
        Run/Turn/ModelCall/StateChanged, và thực hiện đúng một lệnh gọi model đồng bộ, không
        streaming, chưa bao giờ chạm tới tool pipeline, chứ đừng nói tới một sandbox exec.
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
              <ApiDiagnosticNote>{t.s1RecoNote}</ApiDiagnosticNote>
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
                <ApiDiagnosticNote>{t.grantedNote}</ApiDiagnosticNote>
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
            <CodePanel filename="tests/test_tool_pipeline.cpp">
              {highlightCpp(capabilityDenialErrorSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.denialErrorNote}</p>
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
              <ApiDiagnosticNote>{t.s2Note}</ApiDiagnosticNote>
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
            <ApiDiagnosticNote>{t.s3NoteCite}</ApiDiagnosticNote>
          </RevealItem>
        </RevealGroup>

        {/* ---- 3b. Secret quarantine (ADR-068) ----------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="secret-quarantine" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3bEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3bHeading}</h3>
              <p>{t.s3bBody}</p>
              <ApiDiagnosticNote>{t.s3bHmacNote}</ApiDiagnosticNote>
            </div>
          </RevealItem>

          <RevealItem>
            <p style={{ color: "var(--text-dim)", lineHeight: 1.65, marginBottom: 12 }}>{t.s3bOwnershipIntro}</p>
            <ApiTable
              columns={[...t.s3bOwnershipColumns]}
              templateColumns="1.1fr 0.7fr 2.6fr"
              rows={secretQuarantineOwnership[lang].map((r) => [
                r.concern,
                <code key="owner">{r.owner}</code>,
                r.notes,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <p style={{ color: "var(--text-dim)", lineHeight: 1.65, marginTop: 24, marginBottom: 12 }}>{t.s3bTriggerIntro}</p>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.s3bVerifiedLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.s3bVerified}</p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.s3bAgentLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.s3bAgent}</p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="secret_quarantine.hpp">{highlightCpp(secretQuarantineExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s3bNote}</p>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 14 }}>{t.s3bResidualNote}</p>
          </RevealItem>

          <RevealItem>
            <div style={{ marginTop: 14, display: "flex", gap: 16, flexWrap: "wrap" }}>
              <RawCite href={gh("include/agentengine/trust/secret_quarantine.hpp")} label="include/agentengine/trust/secret_quarantine.hpp" />
              <RawCite href={gh("decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md")} label="decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md" />
              <RawCite href={gh("tests/test_rt_agent_session_quarantine_tool.cpp")} label="tests/test_rt_agent_session_quarantine_tool.cpp" />
            </div>
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
            <ApiDiagnosticNote>{t.s5FindingNote}</ApiDiagnosticNote>
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
            <ApiDiagnosticNote>{t.s6NoteCite}</ApiDiagnosticNote>
          </RevealItem>
        </RevealGroup>

        {/* ---- 7. Remote callback authentication ---------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="remote-callback-auth" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s7Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s7Heading}</h3>
              <p>{t.s7Body}</p>
              <ApiDiagnosticNote>{t.s7Note}</ApiDiagnosticNote>
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
              <ApiDiagnosticNote>{t.s8Note}</ApiDiagnosticNote>
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
              <ApiDiagnosticNote>{t.s9BodyNote}</ApiDiagnosticNote>
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
            <ApiDiagnosticNote>{t.s9NoteCite}</ApiDiagnosticNote>
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
              <ApiDiagnosticNote>{t.s10Note}</ApiDiagnosticNote>
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
