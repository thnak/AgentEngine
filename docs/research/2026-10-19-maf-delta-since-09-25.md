# MAF delta, 2026-09-25 .. 2026-10-19

Sources: `microsoft/agent-framework` `origin/main`, fetched 2026-10-19. Local clone was at `6f1522a50`
(2026-09-25); 325 commits landed since. Releases tagged in the window: .NET 1.25.0 (2026-10-14, #9388),
Python 1.21.0 and 1.22.0.

**Evidence level.** This note reads commit titles and the first lines of commit bodies only; no diffs
were read. Every "relevance to AgentEngine" line is a hypothesis to verify against our spec, not a
finding. Bumps, CI, test-only and doc-only commits were filtered out. Commit IDs are abbreviated
`git log --grep="(#N)"` hits on `origin/main`.

## Items worth a closer read

### AG-UI trust boundary (Python) -- relevant to I2, I3, I7
| PR | Commit | What the message says |
|---|---|---|
| #9413 [BREAKING] | 9a1d66b3e | AG-UI now requires an explicit authorization scope at registration; stable single-user scopes documented. |
| #9368 [BREAKING] | abeed6c62 | AG-UI merges client Shared State into `AgentSession.state` minus a protected set of server-owned namespaces. That set came from optional attributes of the agent object, so a wrapper not exposing them left approval state overwritable. Now protected by shape (non-empty `rules`, `queued_approval_requests`, `collected_approval_responses` lists) as well as by name; also covers approval state held as a `ToolApprovalState` object. |
| #9365 [BREAKING] | 0d5af0fd4 | Workflow runs omitting `threadId` resolve one id up front; it keys the factory cache, the thread snapshot and the live `request_info` interrupts, so a continuation must send it back. Checkpoint ownership still binds only client-supplied ids. |
| #9367 | 95511ff02 | Standalone FIDES session-state key reserved so request/stored Shared State cannot be merged over it. |
| #9316 [BREAKING] | 4a90089f3 | Broad AG-UI trust-boundary hardening; also legacy snapshot media migration and rejecting unresolved media references. |

Hypothesis: the #9368 failure mode (a client-controlled state channel overwriting server-owned
approval state, protected by an allow-list derived from the object being wrapped) is a concrete I3
test case for our AG-UI surface. Verify: can any client-supplied state reach approval state, and is
the protection structural rather than name-based?

### Approvals
- #9152 (.NET, 95feea605): approval requests that never receive a response are rejected automatically.
- #9393 (.NET, 32fc0cdf3): registered function approval requirements are honored.
- #9389 / #9390 (docs): "approval reuse considerations", .NET and Python.

Hypothesis: relevant to the `ApprovalDecider` / `PolicyDecider` conversion (ADR-237 Phase G) -- the
no-response path must fail closed (ADR-070), and approval reuse needs a stated policy.

### MCP 2.x (Python)
- #9236 [BREAKING] (28e984455): moves to `mcp` 2.x; websocket optional is now core; tests for legacy and v2 servers.
- #9237 (docs): v2 client lifecycle and samples; #9451: MCP lifecycle task context isolated.

Hypothesis: compare against the 2026-07-28 stateless-core revision our client targets
(CLAUDE.md research rule: re-verify, do not assert from memory).

### Workflows
- #9432 (.NET, ee3628384): fan-out targets deduplicated -- a target named twice previously ran twice on the same message.
- #9291 (Python, 0cc867d96): concurrent steps preserved/settled across workflow pause; detached steps stopped after pause.
- #9289: restored sub-workflow sibling requests. #9300: switch-case branches may share a target executor. #9394: reused executor state refreshed. #9331: loop inner stream closed on early exit.
- #9269 (.NET): caller cancellation preserved in memory providers.

Hypothesis: edge cases to check against our supervisor, `sub_workflow`, pause/resume and cancellation
(ADR-157, ADR-237 #179). The fan-out duplicate and the pause-with-in-flight-steps cases are the
most likely to have an analogue.

### Content model and telemetry
- #8886 [BREAKING] (8ec38c481): consolidates content identity and protocol metadata (their ADR 0044, per-provider and generic namespaces, explicit stream-merge semantics).
- #9288 (06ebcaac2): exception telemetry redacted when sensitive data is disabled; MCP exception logs gated.
- #9361 (cd3f6c8f2): tool arguments kept off invocation metrics.

Hypothesis: check our OpenTelemetry GenAI spans/metrics for tool arguments and exception text under
the sensitive-data switch (see `2026-08-07-sensitive-input-and-secret-leakage-handling.md`).

### Misc hardening
- #9358 [BREAKING]: declarative HTTP response bodies bounded. #9364: Monty file capture blocks reparse points.
- #9386 (.NET): A2A packages to 1.0.0-preview3; #9465: A2A message validation fixes.

## Not read, deliberately
Provider-specific fixes (Bedrock, Anthropic, Gemini, Cosmos), samples and community-project additions.

## Follow-up
No issues filed from this note yet. Candidates, each needing the diff read first: AG-UI approval-state
protection by shape (I3), approval no-response fail-closed path, fan-out dedup and pause-with-inflight
cases, telemetry redaction audit, MCP 2.x comparison.
