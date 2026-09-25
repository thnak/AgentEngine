# ADR-184 — A text-derived call is never gated less than the same vendor-structured call

- **Status**: **Proposed — design + implementation + proof + red-team pass 1 (2026-09-25; no
  Critical, two Real gaps, both fixed in this ADR, §6).**
- **Date**: 2026-09-25
- **Origin**: ADR-183 §5. While building ADR-183's proof, a tool-call hook that only rewrote a gated
  call's arguments let the call run with no approval at all. Project-owner direction (2026-09-25):
  fix it.
- **Amends**: 007 §4 (the ADR-023 text-derived declassifier amendment); ADR-070 §5a's tripwire
  form ("the `PolicyDecider` is never consulted for a `text_derived` call" becomes "it can never
  approve one", §2).
- **Touches**: `include/agentengine/core/tool_pipeline.hpp` (`tool_call_requires_approval`,
  `resolve_approval_outcome`, `background_task` step 5, and the step-5 comment in `invoke_tool`), and
  `include/agentengine/rt/background_job_runner.hpp` (`BackgroundJobRunner::submit`).

## 1. The defect

```cpp
// before
return (provenance == call_provenance::text_derived)
           ? !is_auto_declassifiable_text_derived_call(tool)
           : (tool.approval != approval_mode::never_require);
```

For a `text_derived` call the tool's own `approval` mode was never read. A pure tool whose capability
ceiling is empty or inert was auto-declassified *even when it declared `always_require` or
`policy_driven`*. "Inert" includes read-only kinds such as `FsRead`. So a file-reading tool whose
author asked for approval on every call was bypassed too: a confidentiality hole, not only a
capability-free one. So lowering a call's trust removed an approval that the higher-trust form of the
same call must pass:

| Tool (pure, empty or inert ceiling) | `vendor_structured` | `text_derived` (before) |
|---|---|---|
| `never_require` | runs | runs |
| `always_require` | needs a decider | **runs, no human** |
| `policy_driven` | `PolicyDecider`, else a decider | **runs, no human** |

`invoke_tool()`'s own step-5 comment stated the intended property ("can only ever be MORE restrictive
than the tool's own setting"), and the code broke it. 007 §4 says the declassifier converts *tainted to
trusted*. A trusted call is still subject to its tool's declared approval, so nothing in the spec
licensed skipping it.

**Who could reach it.** Every path that produces a `text_derived` call:
- a host tool-call hook that rewrites arguments (`enforce_hook_rewritten_tool_call_provenance`);
- middleware that adds calls the backend's response did not contain (`core/middleware.hpp`);
- calls parsed from model text by the response-format leak scan (`response_format_leak_scan.hpp`).
  This is model output, so it touches I3, but the scan is off by default (ADR-023 Finding 6).

**Bound.** Only a tool that is pure with an all-inert capability ceiling qualified. So the bypass never
reached an egress or mutation capability. It skipped a human decision the tool's author had asked for,
which is itself a violation of 006's approval contract.

## 2. Decision

```cpp
// after
bool const tool_requires = tool.approval != approval_mode::never_require;
if (provenance == call_provenance::text_derived) {
    return tool_requires || !is_auto_declassifiable_text_derived_call(tool);
}
return tool_requires;
```

**A `text_derived` call is never gated less than the same `vendor_structured` call.** The
declassifier lifts only the gate that text-derived provenance adds; the tool's own gate always
applies. The ADR-023 override is unchanged: `never_require` on a non-declassifiable tool still needs
approval for a `text_derived` call.

### `policy_driven`

A `vendor_structured` call to a `policy_driven` tool consults the host's `PolicyDecider` first.
ADR-070 bars the `PolicyDecider` from `text_derived` calls ("007 §4's closed declassifier list stays
closed"), and this ADR keeps that bar. So a `text_derived` call to a pure, capability-free
`policy_driven` tool now needs a real `ApprovalDecider`, or a suspend for a human. That is
*stricter* than the vendor path when the policy would auto-approve. It fails safe, and it is the only
option that neither reopens ADR-070's barrier nor keeps the bypass. A host that wants text-derived
calls to such a tool auto-approved can wire an `ApprovalDecider`, which is the seam built for that.

### A host policy's `auto_deny` binds text-derived calls too (red-team, Real gap 1)

With only the predicate fixed, one case was still laxer. For a `policy_driven` tool, a
`vendor_structured` call asks the `PolicyDecider` first, and `auto_deny` refuses it outright. A
`text_derived` call skipped the `PolicyDecider` entirely, fell through to the `ApprovalDecider`, and an
allow-by-name decider approved it. Concretely: `make_plan_execute_policy_decider` (which auto-denies
non-planning-safe tools until the plan gate opens) plus `QuickstartSessionBuilder::approve_tools({...})`
let a text-derived `write_file` run before the plan gate opened, while its vendor twin was refused.

`resolve_approval_outcome` now consults the `PolicyDecider` for a `text_derived` call to a
`policy_driven` tool, and honours **only** `auto_deny`; `auto_approve` and `require_approval` fall
through to the approval gate. A deny only narrows, so ADR-070's concern (a host policy *loosening* a
text-derived call past 007 §4's closed declassifier list) stays closed. What changes is ADR-070 §5a's
proof shape: its tripwire asserted "never consulted", and now asserts "an `auto_approve` never lets it
through".

### Background paths used their own check (red-team, Real gap 2)

`background_task()` step 5 and `BackgroundJobRunner::submit` each tested `tool.approval !=
never_require` directly, ignoring provenance. On those paths a `text_derived` call to a `never_require`
tool with a real capability (e.g. `NetOut`) was backgrounded with no approval, so ADR-023's override
never applied there. Both now call `tool_call_requires_approval(tool, request.provenance)`. Neither
path consults a `PolicyDecider`, as before. Reach is limited: `start_background_task` and `submit`
have no product callers today, only host code and tests. ADR-070 had named this path as a residual.

### Behaviour change

Affected, all stricter:
- `text_derived` calls to pure, inert-ceiling tools that declare `always_require` or `policy_driven`
  now need approval where before they ran unasked;
- `text_derived` calls to `policy_driven` tools are now refused when the host's policy says
  `auto_deny`, where before an `ApprovalDecider` could approve them;
- `text_derived` background calls now get ADR-023's override.

Nothing becomes more permissive. `vendor_structured` behaviour is byte-for-byte unchanged (T3, and the existing
ADR-023 P2-T3 cases). The default approval mode for a C++ tool is `never_require`
(`tool_descriptor.hpp`), which is unaffected, so the affected set is tools whose authors *explicitly*
asked for approval.

## 3. Proof

- **`tests/test_tool_pipeline.cpp`, ADR-184 T1–T6** (new):
  - T1: a `text_derived` call to a pure, capability-free `always_require` tool is refused with no
    decider.
  - T2: the decider is consulted for it, a "no" blocks it, and a "yes" lets it through.
  - T3: parity; the same call `vendor_structured` is refused with no decider (unchanged).
  - T4: `text_derived` to a pure, capability-free `policy_driven` tool is refused with no
    `ApprovalDecider`, and a tripwire `PolicyDecider` is never consulted.
  - T5: `text_derived` to a pure, capability-free `never_require` tool still auto-declassifies.
  - T6: over every approval mode on a declassifiable tool, `tool_call_requires_approval` is never
    laxer for `text_derived` than for `vendor_structured`.
  - T7: a `PolicyDecider` that says `auto_deny` plus an allow-all `ApprovalDecider`: both the
    `vendor_structured` and the `text_derived` call are refused as `tool.policy_denied`, and the
    `ApprovalDecider` is never consulted.
  - T8: a pure `always_require` tool whose only capability is `FsRead` is declassifiable, and still
    requires approval for a `text_derived` call.
  - T9: `background_task` refuses a `text_derived` call to a `never_require` `NetOut` tool with no
    decider, synchronously, before any thread starts.
  - T1 and T4 pin `tool.approval_denied`, and T4 shows that an approving `ApprovalDecider` lets the
    call through, so the refusal is the gate and not an unrelated failure.
  - ADR-070's text-derived case now asserts that the `PolicyDecider`'s `auto_approve` never lets the
    call through (refused as `tool.approval_denied`).
- **`tests/test_rt_background_job_runner.cpp`, R1 (ADR-184)** (new): a `text_derived` request to a
  non-declassifiable `never_require` tool needs attestation; the same request `vendor_structured` does
  not.
- **`tests/test_rt_agent_session_tool_call_hook.cpp`, H5** (new): ADR-183 §5's exact reproduction,
  end to end. A hook rewrites a pure `always_require` call's arguments, the round suspends for
  approval, the tool does not run before approval, and it runs once approved.
- **`tests/test_rt_agent_session_approval_resolved_order.cpp`**: its gated tool is back to `pure`,
  so O4/O5 (hook rewrite, then approve or deny) now cross this path too.
- **Positive controls** (pre-fix headers stashed):
  - `test_tool_pipeline`: T1, T2, T4, T6, T7 (text-derived), T8 and T9 fail. T3, T5 and T7's
    vendor twin pass either way, as they should, because they check behaviour that must not change.
  - `test_rt_agent_session_tool_call_hook`: H5 fails.
  - `test_rt_agent_session_approval_resolved_order`: O4/O5 fail with "suspends for approval (ran to
    completion)".
  - `test_rt_background_job_runner`: the new R1 text-derived check fails.
- **Existing suites**: every existing ADR-023 case in `test_tool_pipeline` passes unchanged; ADR-070's
  text-derived case was restated (§2). Full suite in §5.

## 4. Residuals

- The `policy_driven` + `text_derived` + declassifiable case is now stricter than its vendor twin
  (§2). This is intended; revisit only with a design that lets a `PolicyDecider` see provenance without
  reopening ADR-070's barrier.
- The leak scan and middleware paths were not exercised end to end here. The red-team traced every
  approval decision to the shared predicate:
  - `invoke_tool` and the ADR-160 batch admission;
  - the suspend pre-check and the cascade after a hook decision;
  - agent-as-tool;
  - MCP `tools/call`;
  - the CodeAct replay;
  - the two background paths, since this ADR.

  T1–T9 pin that predicate.

## 5. Full offline suite

**314/314** (`ctest -j 8`, Windows, Debug, 2026-09-25), after the red-team fixes.

## 6. Red-team pass 1 (2026-09-25, `general-purpose` agent, no prior context)

No Critical findings.

**Verified correct by the red-team:**
- The new predicate matches §2, and `vendor_structured` is unchanged.
- The ADR-023 override is kept, and the `PolicyDecider` cannot approve a `text_derived` call.
- Every approval decision in the tree goes through the shared predicate, except the two background
  paths (Real gap 2).
- No shipped flow breaks. First-party pure tools are all `never_require`. Agent-as-tool is
  `at_most_once`, so it is not declassifiable. The plan-execute gate's default decider returns
  `require_approval`, so a `text_derived` call gets the same result as its vendor twin there.
- H5 and O4/O5 exercise the hook-rewrite path.

**Real gaps, both fixed here:**
1. A host policy's `auto_deny` did not bind `text_derived` calls (§2).
2. The background paths ignored provenance (§2).

**Minor, addressed:**
- The `FsRead` (confidentiality) case is now stated in §1 and tested (T8).
- T1 and T4 now pin their error codes.
