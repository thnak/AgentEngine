# ADR-236: Recording and replaying a batched run's vendor calls and clock (I5)

**Status: Proposed (2026-10-03).** Closes two ADR-235 §7 residuals: "No recorded seam for replaying batch
results (I5)" and "`tools/batch_infer.cpp` still targets the beta path". Red-teamed once against the code
(§5: 0 FATAL, 4 MAJOR, all fixed). One finding needed a small engine change to `WorkflowSupervisor::execute()`
(§3.1).

## 1. The question

I5 says nondeterminism crosses a recorded seam. ADR-235 put two new sources of it into a workflow run:

- **the vendor's answers:** what each `poll()` returns, when a job ends, which items fail;
- **the clock:** `BatchPolicy::now_ns` decides whether a poll error is forgiven (`poll_error_grace`) and when a
  job is given up on (`max_wait`).

ADR-235 made the clock injectable but recorded neither. A batched run could not be reproduced offline, so a
bug report against one could not be replayed, and a fix could not be checked against the run that exposed it.

A synchronous model call already has this seam: `RecordingChatClient` / `ReplayChatClient`
(`core/chat_recording.hpp`, 004 §6). The question is what the batch equivalent must look like.

## 2. Constraints

1. **Replay must not hand recorded answers to a different run.** A batch result is matched to its node by
   custom id alone (ADR-235 §3.5). `ReplayChatClient` ignores the live request; doing the same here would let
   a replay fed different input complete, with every node receiving answers generated for prompts it never
   sent. The run would look correct.
2. **The clock and the backend interleave.** `poll_error_grace` is decided by the clock reading taken during the
   same `poll_batches()` as the poll. Replaying each in its own stream loses which reading belonged to which
   poll.
3. **A batched run outlives its process.** It suspends for minutes to hours and is restored elsewhere
   (ADR-235 §3.4). A recording must survive that, and must not become something the restore path reads.
4. **Recordings carry resolved results.** ADR-235 §3.4 keeps those out of every `RunStateRecord`, so that
   editing a checkpoint cannot forge an output. A recording must not open a way around that rule.
5. **No new authority (I2).** Replay is a host's choice, like `ReplayChatClient`. Nothing in the engine may load
   a recording on its own.

## 3. Decision

`core/batch_recording.hpp` (bodies in `src/core/batch_recording.cpp`):

- **`BatchRecorder`** owns **one ordered stream per run**.
  - `wrap(backend)` returns a `BatchBackend` that records every `admit`, `submit`, `poll`, `cancel` and
    `release`: its inputs, and its outcome (value, error, or the text of what it threw). It also writes a
    `backend` header carrying the group key and limits.
  - `clock(inner)` returns a `now_ns` that records each reading into the same stream.
  - Records go to a host sink, in call order, under the recorder's lock. `group_key()` and `limits()` are
    configuration and are not recorded as calls.
  - **Transparent:** a wrapped backend returns exactly what its inner backend returned and rethrows what it
    threw (claim R2). A failure to *record* never becomes a failure of the call: if the sink throws, or
    building the record runs out of memory, the call's real outcome is returned and `failed()` turns true
    (R14). Otherwise a sink failure after a successful `submit` would read as a failed submit and orphan a
    paid job.
- **`BatchReplayer`** answers from a recording, **checking every call** against the record it is about to
  answer with:
  - the operation;
  - the backend's group key;
  - the job id (`poll`, `cancel`, `release`);
  - the principal and tenant the call ran under (`submit`, `poll`, `cancel`, `release`; I4);
  - the request, as `batch_request_to_json`, compared byte for byte (`admit`; for `submit`, every item's custom
    id and request, in order). That encoding is 004 §6's `chat_request_to_json` plus the two fields it leaves
    out, `reasoning_effort` and `client_interaction_answers`. A structured-binding arity check in the `.cpp`
    breaks the build when `ChatRequest` gains a field, so the encoding cannot quietly fall behind.

  Any mismatch **latches** the replayer as diverged. That call and every later one fail with class `contract`
  (`batch_replay.diverged`, or `batch_replay.exhausted` past the end), and the replay clock stops advancing.
  `diverged()`, `divergence()` and `remaining()` let a test assert that a replay was exact and complete.
- **A divergence stops the run.** `on_divergence(hook)` runs once, outside the replayer's lock. A replay host
  passes `[&sup](auto const&) { sup.cancel(); }`. This is needed because, to the supervisor, a failed `admit`
  means "not batchable" and a failed `submit` means "send it synchronously" (ADR-235 §3.3). Without the
  hook, a divergence turns into live synchronous model calls and a run that completes on fresh answers.
- **File form: JSON Lines**, appended one record per call (`append_batch_call_record`), so a restored process
  wrapping its backend again keeps adding to the same file. Repeated identical headers are one backend; a key
  given two different limit sets is refused. `now_ns` is written as a decimal string, because nanoseconds
  since the epoch exceed a double's exact range.

**Why replay compares the request when `ReplayChatClient` does not.** In a chat replay the answer goes back to
the one caller that asked. In a batch replay it goes to whichever node holds the matching custom id, and
custom ids are index-only (ADR-235 C3), so they say nothing about which prompt was asked. Comparing the
request is the only thing that binds a replayed answer to the question it was generated for. Mutant M1 (§6)
removes that check, and R4 then shows recorded answers reaching a run fed different input.

**Why one stream instead of a per-job lookup.** The supervisor's batch code iterates vectors only
(`src/rt/workflow_supervisor_batch.cpp`), so its call order is a function of the run's inputs. A strict
sequence is therefore reproducible, and it is the only form that keeps constraint 2: which clock reading
came with which poll.

**What a recording is not.**
- It is not a checkpoint. No restore path reads it.
- It confers no authority. A host builds a `BatchReplayer` from a file it chose and passes its backend to a
  node's `BatchableModelCall`, exactly as it would pass a `ReplayChatClient`.
- It never holds a credential. A backend resolves its secret inside `submit()`/`poll()`, and the recorder
  sees only the arguments and the return value.

### 3.1 The engine change: a cancel during the batch gather stops the round

The hook first proved insufficient. R4's replay was cancelled, but the round still ran its synchronous
fallbacks. `execute()` checked for cancellation at the top of a round, between retries and after the wave, but
not between the batch gather (where the hook fires) and the first dispatch. It also submitted the round's batch
chunks before its post-wave cancel check, so a cancel during a round still paid for a job that the next poll
would only abandon.

`execute()` (`src/rt/workflow_supervisor.cpp`) gains two guards:

- **Before the first wave:** if cancel was requested, nothing dispatches. Each undispatched delivery gets a
  failed reply, which is never folded, and the existing post-wave check ends the run `cancelled` before routing.
  This is the same treatment issue #156 gives a cancel observed mid-wave.
- **Before submit:** a cancelled round submits no batch chunk.

The guards apply to every cancel, not only to replay. A host thread that cancels while a round gathers now gets
neither the synchronous calls nor a paid job. ADR-235 claim C23 proves this directly in the coalescing suite;
before the change, that test submitted the job.

Editing a recording changes what a replay answers. That is equivalent to editing a chat recording, and it is
the host's own input. It is not a way around ADR-235 §3.4, which protects the engine's own checkpoint.

## 4. `tools/batch_infer.cpp`

The tool drives the engine's `openai::OpenRouterBatchBackend` through the `BatchBackend` seam:
`admit` → `submit` → `poll` (with the same post-submit grace the engine uses) → `release`. It replaces the
hand-written beta-path client, so the tool and a batched workflow round speak the vendor through one wire
translation. `AGENTENGINE_BATCH_RECORDING=<path>` records the run through `BatchRecorder`.

What changed in behaviour:
- **One job, not two.** The old tool also submitted an Anthropic `/v1/messages`-shaped job. The engine
  backend speaks only the chat-completions shape (ADR-235 §3.2), so that job went with the hand-written
  client. A probe on 2026-10-03 found that OpenRouter's GA endpoint does **accept** a `/v1/messages` job:
  `POST /api/v1/batches` returned `validating`, and the job completed about 12 minutes later. Its result body was an
  Anthropic `message` (`type: "message"`, `content: [{type: "text", ...}]`, `stop_reason`, `usage.input_tokens`),
  the shape `anthropic::detail::parse_message_response` reads. Speaking that shape from the engine would be a
  second backend shape, not a tool change.
- **New default model.** `google/gemini-2.5-flash-lite`, the model the GA endpoint accepted on 2026-10-02. The
  old comment calling `openai/gpt-4o-mini` "confirmed batch-capable" described the beta; the GA endpoint
  refused it for this account.
- **Key variable.** It reads `AGENTENGINE_OPENROUTER_BATCH_API_KEY` first, the variable the live batch test
  uses, and falls back to `AGENTENGINE_OPENROUTER_API_KEY`.

## 5. Red-team (independent adversarial pass against the code, 2026-10-03)

An agent prompted to break the design read the code and reported 0 FATAL, 4 MAJOR and 15 MINOR findings. It
confirmed four things: no engine restore path reads a recording; no credential reaches one; `now_ns` is called
only at three unconditional sites; and all batch iteration is over vectors.

| # | Finding | Resolution |
|---|---|---|
| MAJOR 1 | `chat_request_to_json` omits `reasoning_effort` and `client_interaction_answers`. A replay whose `build()` began setting one still matched, so recorded answers reached a request they were not generated for | `batch_request_to_json` plus the arity guard (§3); R12 |
| MAJOR 2 | A divergence fell back to synchronous calls. The replayed run made live model calls and completed (status 0), and R4 had accepted that | `on_divergence` + `cancel()`, and the engine guards (§3.1); R4 now requires `cancelled` and zero synchronous calls; C23 |
| MAJOR 3 | A throwing sink after a successful `submit` read as a failed submit (orphaning a paid job), and a throw from the clock wrapper escaped a round | `emit()` never throws; `failed()` reports it; R14 |
| MAJOR 4 | `read_batch_recording` used the parser's default 100,000-node budget. A poll of about 4,500 items wrote fine and could not be read back | `kBatchRecordingParseBudget`, sized for a 50,000-item job; R15 (10,000 items; the default budget refuses the same line) |
| minor | A torn final line made the file unreadable, and the next append merged into it | the torn final line is skipped on read; an append after one starts a new line, and the fragment is then refused by line number, never merged; R16 |
| minor | The principal was neither recorded nor compared | recorded and compared (I4); R13 |
| minor | A non-standard throw replayed as a `std::runtime_error`, changing the supervisor's event text | replayed as a non-standard throw; R17 |
| minor | Several test accesses read through an empty `result`/vector (a crash, not a failed check); the R3 control did not assert it stayed in step | guarded by `replay_rig`; the control asserts `!diverged()` and a `suspended` end |
| minor | `batch_infer`: `std::stoi` on a bad wait value terminated; it gave up after one aged poll error where the engine allows `max_poll_errors`; `OK` counted results, not ids; it appended across runs | `from_chars`; 5 consecutive errors; every sent id needs exactly one succeeded result; refuses a non-empty recording file |
| minor | Limits are snapshotted at `wrap()`; `deadline_ms` uses `steady_clock`, not the recorded clock; after a divergence the replay clock repeats its last value | residuals (§7) |

## 6. Falsifiable claims and evidence

`tests/workflow/test_rt_workflow_batch_replay.cpp`, run against the real `WorkflowSupervisor` with
`testing::ScriptedBatchBackend` as the vendor while recording, and no vendor at all while replaying.

| # | Claim | Disproved if |
|---|---|---|
| R1 | A recorded run (submit, a transient poll failure, a partial result, an errored item that falls back to a synchronous call, job end, release) written to a JSON Lines file and read back replays to identical results and event kinds, never diverging, consuming every record | any result or event differs, or records remain |
| R2 | Recording is transparent: the recorded run equals the same run unrecorded | they differ |
| R3 | A clock-decided outcome replays. Poll errors are forgiven inside `poll_error_grace`; the job fails closed once aged. The replay fails closed at the same poll. **Control:** the same backend answers with a clock that is not the recorded one do not fail closed | the replay differs, or the control reaches the recorded outcome |
| R4 | A replay fed different input diverges at the first differing call (`admit`), and no recorded answer reaches the run | it does not diverge, or a recorded answer appears |
| R5 | A recording whose poll names another job id diverges at that poll, and the run does not complete | it completes |
| R6 | A truncated recording diverges as exhausted, and every later call also fails | a later call is served |
| R7 | A run checkpointed, restored into a fresh supervisor with a fresh recorder on the same file, then polled, replays from that one file through the same restore to identical results | they differ |
| R8 | A backend that threw is recorded with its message and replays as a throw at the same call, with identical results | the throw is lost or the run differs |
| R9 | The codec round-trips a poll with succeeded and errored items, a clock reading above 2^53 exactly, and refuses a malformed line by number | any field is lost, or the bad line is accepted |
| R10 | A recording that gives one key two limit sets, or a call record with no group key, is refused; an unknown backend is refused; a replay backend reports the recorded limits | any is accepted |
| R11 | A real OpenRouter run recorded by `batch_infer` (`tests/fixtures/batch/`) replays offline: 3 admits, the submit, the vendor's real post-submit 404 as a poll error, every poll to the ended job, the release, and each prompt's parsed answer matched by custom id (the vendor returned them out of order) | any call diverges, or an answer differs |
| R12 | A request differing only in `reasoning_effort` or in `client_interaction_answers` diverges at `admit`. **Control:** the identical request replays | either is served, or the control diverges |
| R13 | A `submit` under another principal, or another tenant, diverges. **Control:** the recorded principal replays | either is served |
| R14 | A sink that throws leaves a vendor-accepted `submit` returning its job id (one vendor submit) and a clock read not throwing; `failed()` is true. **Control:** a vendor-refused submit is still reported refused | the submit reports failure, or a throw escapes |
| R15 | A 10,000-item poll record appends and reads back whole. **Control:** the parser's default budget refuses the same line | it does not read back |
| R16 | A torn final line is skipped; after another append, the fragment is refused by line number instead of being merged. **Control:** an unterminated final line that parses is kept | the torn line is merged or the whole record lost |
| R17 | A recorded non-standard throw replays as a non-standard throw | it replays as a `std::exception` |
| C23 | (coalescing suite) A cancel during the batch gather ends the run `cancelled` with no job submitted and nothing pending or abandoned. **Control:** without the cancel, one job is submitted | a job is submitted |

**Result:** every check passes on MSVC Debug. That is 63 replay checks; the coalescing suite, with C23 added;
and the cancellation and supervisor suites, which are unchanged by the engine guard.

**Mutants:** each guard was removed in turn and the suite re-run. Every mutant was killed.

| Mutant | Killed by |
|---|---|
| M1 request comparison removed (`admit` and `submit`) | R4 (7 checks): recorded answers reached a run fed different input |
| M2 job-id comparison removed | R5 (2) |
| M3 replay clock ignores the recording | R3 (2) |
| M4 divergence not latched | R5, R6 |
| M5 recorder drops a thrown call | R8 (2) |
| M6 `reasoning_effort` not encoded | R12 |
| M7 principal not compared | R13 (2) |
| M8 sink exceptions propagate | R14 (the clock). `record()`'s own catch still protects `submit`: two layers, the outer one proven |
| M9 default parse budget | R15 |
| M10 torn final line not skipped | R16 |
| M11 divergence hook never run | R4 (2), R5 |
| M12 engine: pre-dispatch cancel guard removed | R4: a synchronous call ran |
| M13 engine: pre-submit cancel guard removed | C23 (2): the cancelled round submitted a job |

The first pass ran M1-M5 against the pre-red-team code; the second ran all thirteen against the final code.

**Live:** `agentengine_batch_infer` against OpenRouter GA (`google/gemini-2.5-flash-lite`, 3 prompts) on
2026-10-03, through the engine backend: submitted, polled to the end, answers "Paris", "4", "Red.", released.
The run was recorded and committed as R11's fixture, with no credential in the file. (The `/v1/messages` probe
is in §4.)

## 7. Residuals

- **The `/v1/messages` shape on OpenRouter** works end to end (§4), but the engine does not speak it: a batched
  item is built only by the chat-completions translation.
- **A recording is plaintext.** It holds prompts and model answers, like a chat recording. Where it is stored,
  and who can read it, is the host's decision.
- **The synchronous fallback calls of a replayed run** go through the node's own `call`. Replaying them needs a
  `ReplayChatClient` behind that call. This ADR records only the batch seam.
- **`deadline_ms` reads `steady_clock`, not the recorded clock.** A replay that runs faster or slower than the
  recorded run can cross a deadline in a different round. It then diverges loudly; it never answers wrongly.
- **Limits are snapshotted at `wrap()`.** A recording that spans a configuration change of a backend's limits is
  refused by `from_records` rather than replayed.
- **After a divergence the replay clock repeats its last value.** A host loop that polls "until not suspended"
  must check `diverged()`, or set the hook, which cancels the run.
- **A record is written after its call returns.** A crash between a vendor submit and its record loses that
  record. The replay then diverges or runs out; it never answers wrongly.
