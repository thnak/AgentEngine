# ADR-177 — A stream that dies mid-answer: retry the model call, and what that costs

- **Status**: **Proposed — implemented and proven (§9); awaiting a fresh-adversary red-team round and
  project-owner sign-off.** Round 1 (§7) was the author's own read of the code; §9 is executed evidence.
- **Date**: 2026-09-21
- **Origin**: a real interactive `cli_chat` session (2026-09-18) lost a 93 s model call:
  `chat_stream() did not reach a clean terminal`. The provider streamed 526 reasoning chunks in 3.2 s,
  went silent, and `net_egress_proxy.cpp`'s `kIoTimeoutMs` (90 s) fired. The one-line diagnosis
  (dropped `fail_error()`, PR #86) is shipped; this ADR is about what to *do* about it.
- **Refines** ADR-034 §7a (streaming is a per-session opt-in). **Reopens** ADR-034's "streaming gives
  up failover" trade for exactly one shape of failure, and says why the other shapes stay closed.
- **Touches invariants**: I3, I5, I8 (all three matter), I4.

## 1. The finding that shapes everything: the retry already exists, and would not have helped

`ModelCallGateway::call_stream()` already retries a streaming call with backoff and a breaker
(`stream_attempt_with_retry`, `core/model_call_gateway.hpp`). It is **commit-gated**: once any chunk
has been pushed to the caller (`any_pushed`), a failure is terminal — deliberately, because a retry or
a fallback tier after the caller has seen tokens would be a silent mid-answer backend substitution
(004 §4 forbids it).

The incident stalled **after** 526 reasoning chunks had been shown. So even a session wired to that
gateway would have died exactly as it did. The gap is not "no retry"; it is "no answer for a failure
after the commit point". Anything this ADR adds must be honest about that rule rather than route
around it.

Also: the CLI uses a raw `ChatClientT`, not a gateway, so it has no retry at all today.

## 2. Decision (proposed)

A **session-level, opt-in, same-client retry of the whole model call**, bounded per run.

| Aspect | Decision |
|---|---|
| Where | `AgentSession::run_rounds()` around `run_model_call()`. Not inside the drain, not in the transport. |
| Opt-in | `set_stream_retries(n)`, default **0** (bit-for-bit today's behaviour — ADR-034's own C1 precedent). Clamped to a compile-time ceiling of 3. `cli_chat` sets 1. |
| Scope | Only when `!ModelCallGatewayLike<ChatClientT>` and the call streamed (`stream_model_calls_`). Gateway sessions are untouched: their own commit rule stands. |
| Predicate | `error.code == "run.stream_incomplete"` **and** the inner stream failure was `transient`. Nothing else: not `run.usage_unavailable` (a *completed* stream — retrying re-bills it), not contract/policy/fatal, not cancellation. |
| Bound | A **per-run** counter, not per-round. A 40-round run cannot spend 40 retries. |
| Visibility | No new event kind. Each attempt is a complete `model_call_started … model_call_finished` bracket; between them one `warning` event, code `run.model_call_retry`, carrying attempt number and the inner error. |
| History | A failed attempt appends **nothing** to `history_`; the retry re-sends the identical request. |

### Why not the alternatives

- **Change the gateway's commit rule.** Rejected: it exists for a real reason (004 §4), and the
  substitution risk is real there. A *same-client* retry does not substitute a backend; a fallback tier
  would.
- **Add a `model_delta_discard` event so consumers erase the partial.** Rejected for v1: it is a
  013 §1 vocabulary change (I7 gate: AG-UI mapping, conformance), and the partial output was *real
  output the user saw*. Two closed brackets is honest; erasing is a UI policy that belongs to a
  consumer. Named residual (§6).
- **Retry inside `drain_streaming_response`.** Rejected: it has already emitted deltas and owns no
  request to resend.
- **Just shorten `kIoTimeoutMs`.** Orthogonal, not a fix: it shortens the wait for a dead stream, it
  does not recover the run.

## 3. Why this is safe to retry at all (the argument that must survive red-team)

No effect happens before a clean terminal. `run_rounds()` executes tool calls only *after*
`run_model_call()` returns a `ChatResponse`; a stream that fails returns an `error` and the round
ends. So a retried attempt cannot double-execute anything — **provided** no path executes a tool
from a partially streamed `ToolCall`. That proviso is R1 below and is the first thing to attack.

## 4. What it costs, stated plainly

- **I8 (budgets).** A failed stream reports no `Usage`; `run_tokens_consumed_` is charged only on
  success. So today a failed call is already invisible to the token budget, and a retry multiplies
  that blind spot by up to `1 + n`. The retry cap bounds the *count*, not the tokens. Whether a
  provider bills a stream it never finished is an **external claim this ADR does not make** — it
  needs dated, cited research (`docs/research/`) before §6 can be closed.
- **Wall clock.** A stalled attempt costs up to 90 s. A retry that stalls again doubles the wait for
  the same failure. The single incident is one data point; it does not show that a second attempt
  succeeds.
- **I5.** `RecordingChatClient` records per `chat_stream()` call, so two attempts are two records.
  The retry decision must therefore be a pure function of the recorded error and the attempt count —
  **no clock read, no jitter in the decision**. A deadline check would smuggle `steady_clock` into
  control flow.
- **Duplicated prefix.** A consumer sees attempt 1's partial text, then attempt 2's full text.

## 5. Claims to attack (each needs a proof AND a positive control that can fail)

| # | Claim | Attack / positive control |
|---|---|---|
| R1 | A failed attempt executes no tool — even one whose `ToolCall` was **fully streamed** before the stream died | A tool with an invocation counter; a fake stream that emits a complete call then fails. Counter must stay 0. Mutant: assemble/execute on partial → must fail. |
| R2 | A failed attempt leaves `history_` byte-identical | Compare history before/after; mutant that appends the partial. |
| R3 | The cap holds across rounds and a permanently failing stream ends after exactly `n+1` attempts | Mutant with the cap check removed loops — run under `memory_cap.hpp` + watchdog. |
| R4 | **I3**: model text cannot trigger or suppress a retry | Model streams the literal string `run.stream_incomplete` / a fake error-shaped tool result; retry count must not move. |
| R5 | Default (0) is bit-for-bit today's behaviour | The existing streaming suite (S1–S4, 16 checks) unmodified. |
| R6 | Gateway sessions compile and behave unchanged | `if constexpr` — but ADR-176's accidental-`if constexpr` lesson applies: assert it, don't assume it. |
| R7 | Cancellation and approval-suspend never retry | Cancel during attempt 1; suspend/resume across a retry. |
| R8 | A `run.usage_unavailable` (completed stream) is **not** retried | Retrying it silently re-bills a finished call. Positive control needed. |
| R9 | AG-UI projection stays well-formed across two brackets: no dangling `REASONING_*`, monotonic `seq` | Feed the recorded event sequence through `agui::projection`. |
| R10 | A retry that succeeds adds only the successful attempt's usage | No double count, and — separately — no claim that nothing was undercounted. |
| R11 | Replay: a two-attempt recording replays to the same final result | `ReplayChatClient` holds a single recording today; if it cannot advance across two `chat_stream()` calls this is a **finding**, not a test to weaken. |

## 6. Residuals named up front

1. Token cost of a discarded attempt is invisible to the budget (needs provider-billing research).
2. Consumers cannot erase a discarded partial (no discard event; 013 change deferred).
3. Gateway sessions still die post-commit; a *tier-aware* answer is separate, larger work.
4. Pre-first-byte failures (rate limit, refused connect) want backoff; this ADR retries them without
   it or excludes them — undecided (R-question for the red-team).
5. The retry's own delay is a blocking sleep on a pool worker, the same residual
   `stream_attempt_with_retry` already carries.
6. **Live proof is impossible to induce on demand** — a real provider cannot be made to stall. The
   proof is a fake stalling client plus the real timeout path, and this ADR says so rather than
   implying a live run.

## 7. Red-team round 1 (2026-09-21, by the author against the draft, read from the code)

Read, not run. Each row says what the code showed; none is a passing test.

| # | Finding | Effect on the design |
|---|---|---|
| R1 | **Holds structurally.** `history_.push_back(response->message)` (`agent_session.hpp`, after the `if (!response) co_return` in `run_rounds`) and every `invoke_tool()` call site sit *after* `run_model_call()` returns a response. A failed stream never reaches them. | Still needs the counter test with a fully-streamed call; the structure is what the test defends. |
| R11a | **Recording side holds.** `RecordingChatClient` calls its sink once per `chat_stream()` (success *and* failure, with `stream_error_detail`), so two attempts are two records. | None. |
| R11b | **Replay side FAILS — a real finding.** `ReplayChatClient` serves ONE constructor-supplied recording and ignores the request; a second `chat_stream()` replays the *same* recording. A run that failed once and then recovered therefore **cannot be replayed**: replay would fail every attempt while the original succeeded. | I5 is not provable with the existing player. A sequence player (a cursor over N recordings) is a **prerequisite**, not an option, and is new code in its own right. Design change: §2 gains it. |
| Q1 | **The class may not survive.** The TLS layer raises `failure_class::transient` (`src/sandbox/tls_client.cpp`), and the OpenAI client forwards some errors verbatim (`producer.fail(resp.error())`). Whether the *mid-stream* read failure that killed the incident keeps that class, or is re-wrapped on the way to `stream::fail_error()`, was **not verified**. | The predicate rests on a class nobody has checked reaches it. Prove it by driving the real client against a socket that goes silent — not by reading. If it does not survive, the predicate is wrong, not the test. |
| Q2 | `drain_streaming_response()` flattens the inner error into one `transient` `run.stream_incomplete`, so today the retry predicate cannot see the inner class at all. | Needs an out-parameter (internal `detail::`, no public type change). Must **not** be done by matching the message text. |
| Q3 | With `stream_model_calls_` **off** on a raw client there is no retry either way, and the failure is a `chat()` error, not `run.stream_incomplete`. | Scope sentence stays "streamed calls only"; the un-streamed path is explicitly out of scope, not overlooked. |
| Q4 | Pre-first-byte failures (rate limit, refused connect) are transient too, but an immediate retry hammers a provider that just said "slow down". | Decision needed before code: retry only *mid-stream* failures (bytes were received, then silence) for v1, and leave pre-first-byte to a gateway. Narrower, matches the incident, avoids inventing a backoff policy here. **Proposed: mid-stream only.** |

**Status after round 1:** the design survives structurally (R1, R11a), but two things it depended on
are not true as drafted — the replay seam (R11b) and an unverified error class (Q1). Neither is a
reason to abandon it; both are reasons this ADR must not be Judged until they are proven.


## 8. What changed from the draft while building it

- **The `warning` event has no code field**, only a message, so §2's "code `run.model_call_retry`" was
  not expressible. The retry is announced by a message with a fixed prefix
  (`detail::kStreamRetryWarningPrefix`, one constant shared by the emitter and every reader). It is for
  DISPLAY only; no decision reads it.
- **Predicate gained two clauses** the draft missed: `net.cancelled` is classed `transient` by the
  transport (the class alone would retry a cancellation), and the run's own stop token is checked (Q5).
- **`ReplayChatClient` grew a sequence constructor** (R11b) and **`ChatCallRecording` grew
  `stream_error`** (class + code + message). Found while building it: the player rebuilt any recorded
  failure as `fatal`, so even a two-recording tape would have replayed the failed attempt as
  non-retryable and diverged from the run that recorded it. Older recordings (message text only) fall
  back to the old reconstruction rather than a guessed class.
- **The retry re-sends the identical `ChatRequest` object**, so a stable `idempotency_key`
  (`ChatRequest`, F1) is stable across attempts by construction.

## 9. Evidence

`tests/test_rt_agent_session_stream_retry.cpp` (P1-P10, every claim with a control) and
`tests/test_stream_retry_real_transport.cpp` (Q1 on the real client and transport).

**A REAL DEFECT, found by running Q1 rather than reading it.** A connection cut mid-body was not
reported as a stream failure at all. The transport hands a peer close to its caller as an ordinary
end-of-body ("the terminal 0-chunk is the CALLER's to notice"), and neither provider worker noticed:
the stream closed with no usage and the session reported `run.usage_unavailable` — a *contract* failure
nothing retries — for what was a transient one. `ChunkedBodyDecoder::complete()` existed and nothing
called it. Both OpenAI and Anthropic workers now fail a chunk-framed body that ended before its final
chunk with `transient` / `net.stream_truncated`. The run before the fix was red; after, green — that is
the positive control. Unchunked SSE is untouched (its end IS the connection close).

Mutants planted in the real code, each caught: no bound (4 checks), `net.cancelled` exclusion removed
(1), first-byte rule removed (1), class check removed (2). **One survived and is disclosed:** removing
the gateway guard alone changes nothing, because a gateway session never populates the failure detail
either — two independent mechanisms block it. Removing BOTH is caught by P10 (2 checks). So the
gateway exclusion is defence in depth, and P10 proves the outcome, not which layer produced it.

Windows MSVC: the full deterministic suite passes (the Docker-dependent tests need the daemon up and
were run with it). Live, against DeepSeek `deepseek-flash`: OC, reasoning-delta, HITL and session-builder
live tests pass, so a healthy chunked TLS stream is not misflagged truncated; the CLI answers normally
with retries on.

**Code-review follow-up (same PR).** A review pointed out that the truncation check as first written
also failed a COMPLETE answer whose body merely closed without the final chunk (some proxies do), which
the session would then retry and pay for twice. It now defers to the stream's own terminal event
(`[DONE]` for OpenAI, `message_stop` for Anthropic). Proven on the OpenAI path with a loopback server
(mutant: dropping the clause is caught); **the Anthropic clause has no loopback test** and rests on the
same one-line shape. Also: `AGENTENGINE_CLI_CHAT_STREAM_RETRIES` is now parsed strictly.

**Closing two of the gaps §10 listed (same PR).**
- **A body shorter than its declared `Content-Length`** was also treated as a clean end. The transport now
  fails it `transient` / `net.stream_truncated` (only when a length was declared; an unframed body still
  ends at the close). Loopback-proven with an honest-`Content-Length` control; mutant caught.
- **The Anthropic path now has its own loopback proof**: a finished answer (`message_stop` seen) with an
  unterminated body is kept and not retried; a stream cut before `message_stop` is retried; retries off
  fails it. This exposed a real bug in the predicate as first written: `any_update_seen` measures updates
  DELIVERED, but the provider workers hold updates back until a block completes, so a stream cut after a
  200 head and before the first finished block looked pre-first-byte and was not retried. The predicate is
  now "an update was seen OR the failure is `net.stream_truncated`" (that code can only occur after a
  successful head, so it is mid-response by construction). Mutant of the new clause caught by 2 checks.
  A silent provider before its first finished block, with no truncation, is still NOT retried.

**The silent-provider stall (the incident itself), now reproduced and passing.** The read-idle timeout was
two copies of `constexpr 90'000`; it is now one function (`sandbox/io_timeout.hpp`) read once from
`AGENTENGINE_NET_IO_TIMEOUT_MS` (default 90 000 ms, clamped to [250 ms, 10 min], a non-integer keeps the
default). It is host-owned, cannot come from model output (I3), and only changes how long the host waits
(I2 untouched). With it set to 1 s, a loopback server that sends the head and one event and then says
nothing is ended by the transport's REAL idle timeout, retried, and the run converges (took ~1 s, not 90).
Two changes fell out of it:
- A read that fails AFTER the response head was reported as `net.connect_failed`, the same code as a
  refused connection, so "the provider went quiet" and "we never got in" were indistinguishable. Such a
  failure is now `net.stream_read_failed` (only that one transport code is re-coded; a cancellation or a
  byte cap keeps its own).
- The retry predicate counts it as mid-response, so a provider that goes silent BEFORE its first finished
  block (nothing delivered yet) is now retried. Both changes have mutants that are caught.

**I8, researched.** `docs/research/2026-09-21-billing-of-interrupted-streams.md`: no first-party statement
was found (DeepSeek's pricing page is silent, the OpenAI thread is an unanswered question, the litellm PR
cites nothing). A discarded attempt is therefore treated as POSSIBLY billed. The budget cannot enforce a
figure nobody has, so the design bounds the count, announces each retry, and exposes
`stream_retries_used()` for a host that wants its own estimate.

## 10. Still open

1. **Provider billing of an unfinished stream is unmeasured** (above). Closing it needs a first-party
   statement or a killed-stream measurement against a provider dashboard.
2. Consumers still cannot erase a discarded partial (§6.2): the retry replays the answer from the start,
   so a live consumer sees the dead attempt's text and then the full one. Erasing needs a new event kind
   and a 013 change (I7 gate), which is a separate decision.
3. Gateway sessions still die post-commit; a tier-aware answer is separate, larger work (it has to
   reckon with 004 §4's no-silent-substitution rule).
4. Round 2 by a fresh adversary; **Judged is the project owner's to give.**
