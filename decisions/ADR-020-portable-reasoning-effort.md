# ADR-020 — A portable `reasoning_effort`, and why `prompt_cache_key` stays deferred

**Status:** Accepted — 2026-08-07
**Closes:** the Milestone 5 residual "a portable `reasoning_effort` design and `prompt_cache_key` for
Phase D/E's two backends — surveyed, still not built (both explicitly need a further design
decision)", i.e. items 3 and 8 of
`docs/research/2026-08-07-provider-metadata-and-sampling-params-survey.md`'s "Recommended design".
**Amends:** 004-Model-Provider-Plane.md §2 (dated amendment, 2026-08-07).
**Relates to:** 003 §6 (the `Usage::cache_write_tokens` field-ordering lesson), ADR-016 (the live
llama.cpp/OpenRouter reach these probes used).

## 1. Context

The survey left two items explicitly un-built because each needed a design decision rather than an
implementation, and offered three options for the first:

> (a) leave elided permanently, matching temperature/top_p's own status quo; (b) add it as a
> backend-constructor-local field per backend (no portability claim); (c) a real RFC 004 amendment
> defining a portable, coarser tri-state … with each backend mapping down to its own native shape.

The survey's own load-bearing finding argued against a naive version of (c):

> `reasoning_effort` is not one concept with three spellings — OpenAI's flat enum, Anthropic's
> token-budget object, and Ollama's narrower 4-value subset are three genuinely different shapes. A
> portable `ChatRequest.reasoning_effort` field would be exactly "one vendor's shape leaking onto
> every other backend."

That objection is about **admitting a vendor's whole vocabulary**, not about abstraction as such. An
ordinal level narrow enough that no vendor's private values survive it, which each backend must do
real work to map down, is not a leak. §2's measurements are what decided which of (a)/(b)/(c) that
argument actually supports.

## 2. Evidence gathered before deciding (live, 2026-08-07)

Four probes against the two real endpoints this project already reaches.

**(i) Field tolerance is not field support — the control that mattered.** Both `prompt_cache_key`
and an invented `ae_totally_bogus_field_xyz` returned **HTTP 200** from OpenRouter *and* from a
local `llama-server`. Without the bogus-field control, "the endpoint accepted `prompt_cache_key`"
would have read as confirmation; with it, acceptance carries no information at all. This is exactly
the positive-control discipline CLAUDE.md requires — a check that cannot fail proves nothing.

**(ii) `reasoning_effort` could not be shown to do anything on the OpenAI surface.** Comparing
reasoning-trace length across `no field` / `low` / `high` on a reasoning-capable model, the
within-condition variance swamped the between-condition difference (`no field` alone ranged
473–1121 characters). Not evidence of absence — but no evidence of effect either, and combined with
(i), nothing on the OpenAI-compatible surface could distinguish "honoured" from "ignored".

**(iii) The Anthropic mapping *is* observable, because it is structural.** `thinking:{type:
"enabled", budget_tokens:2000}` returned content blocks `['thinking','text']` with
`thinking_tokens: 81`; `thinking:{type:"disabled"}` returned `['text']` with `thinking_tokens: 0` —
same model, same prompt. A field a lenient hop silently drops cannot change the *shape* of a
response.

**(iv) A gateway does not enforce the vendor's constraints.** OpenRouter returned **HTTP 200** for
both `budget_tokens == max_tokens` and `budget_tokens == 512`, each of which `api.anthropic.com`
itself rejects. So a client that relied on the hop to catch a bad mapping would ship a request that
works in testing and fails in production against direct Anthropic.

## 3. Decision

**Option (c), with the portable vocabulary cut to what the evidence supports.**
`enum class reasoning_effort {off, low, medium, high}`, carried as
`std::optional<reasoning_effort>` **appended last** on `ChatRequest` (003 §6's field-ordering lesson,
learned the hard way by `Usage::cache_write_tokens`).

Why (c) rather than (b), given (ii) showed no measurable effect on one of the two surfaces: (iii)
showed a real, observable effect on the other. A per-backend constructor field would mean an agent
author who wants "reason harder" must know which backend is bound and which of two unrelated
spellings to use — and would leave the one backend where the knob demonstrably *works* reachable
only through backend-specific code. The asymmetry in the evidence argues for the abstraction, not
against it.

Four design points, each of which could have gone the other way:

**`minimal` is excluded.** It exists only on OpenAI; Ollama has no equivalent. Admitting it would
make this OpenAI's enum with a rename — the survey's exact objection. Its absence is the thing that
makes this an abstraction rather than a pass-through.

**`nullopt` ≠ `off`.** `nullopt` is *no opinion*: emit no field, take the vendor default — bit-for-bit
today's behaviour, which is what makes the amendment additive for every existing caller. `off` is
*explicitly disable*, which every surveyed backend can express (`"none"`, or
`thinking:{type:"disabled"}`). Collapsing them would make the four-value enum a three-value enum with
a redundant name.

**Levels are gated by the declared `reasoning` capability; `off` is not.** 004 §2's degradation rule
has no *declared* fallback for reasoning effort, and silently dropping the field is precisely the
"silently ignores the request" it forbids — so `low`/`medium`/`high` against a backend without the bit
is a `failure_class::contract` error. `off` is exempt: a backend that cannot reason satisfies "do not
reason" by construction, and gating it would fail a caller who sets `off` defensively across a fleet
of mixed backends, buying no honesty.

**Backends enforce their vendor's own constraints, because (iv) proved no one else will.** Anthropic's
budget is computed as a fraction of the very `max_tokens` the same request carries — 25/50/75%,
clamped up to the documented 1024 floor. Absolute budgets were rejected as a design: they silently
overshoot a small `max_tokens`. `high` deliberately stops at 75% rather than ~100% — the remainder is
what the visible answer is written from.

**`prompt_cache_key` stays deferred, now on evidence rather than on absence of it.** Probe (i)
established that the endpoints available here cannot distinguish support from silent tolerance, and
the decisive check needs `api.openai.com`'s own Chat Completions endpoint, for which this project has
no credential. Building it would mean shipping a field with no gate that could fail — which this
project's conventions specifically forbid. The survey's open verification item is therefore *sharpened*
(from "unconfirmed either way" to "confirmed indistinguishable on the reachable endpoints, decisive
check named"), not closed.

## 4. Falsifiable gates

| # | Claim | What would falsify it |
|---|---|---|
| G1 | The amendment is additive. | `nullopt` emitting any field on either backend. |
| G2 | One portable level, two structurally different native shapes. | Both backends emitting the same kind of thing — a string mapping on each side would mean no abstraction was needed. |
| G3 | The ordinal survives translation. | `low`/`medium`/`high` collapsing to one value, on either backend. |
| G4 | `off` is a request, not the absence of one. | `off` and `nullopt` producing identical wire bytes. |
| G5 | The capability gate fires (negative control). | A level being silently dropped against a backend lacking the `reasoning` bit — or `off` being wrongly refused. |
| G6 | Vendor constraints are enforced client-side (negative control). | An unsatisfiable budget being sent rather than refused; a sub-floor budget not clamping up. |
| G7 | The gate holds on both entry points. | `chat_stream()` accepting what `chat()` refuses. |
| G8 | The level reaches a real model and changes what it produces. | `off` and `high` yielding the same response shape from a live provider. |

G1–G7 are `tests/test_reasoning_effort_portability.cpp` (33 assertions, deterministic, default
suite — both backends driven from the same enumerators in one file, deliberately, since a per-backend
suite could only ever prove half of a portability claim). **G8 is `test_openrouter_live_e2e.cpp`
OR-ANT-8**, measured: **`off` → 0 Reasoning content items, `high` → 1**, same model, same question.
G8 is asserted only on the Anthropic surface, because §2 (i)/(ii) showed the OpenAI surface cannot
carry it — stating that limit rather than papering over it with an acceptance check that proves
nothing.

## 5. Consequences

- **The unsatisfiable-budget branch is not a contrived boundary — it fired on the first real
  configuration it met.** `test_openrouter_live_e2e.cpp` declares `max_output_tokens = 1024` to bound
  cost, and Anthropic's thinking floor is *exactly* 1024 with the budget required strictly below
  `max_tokens`, so no level above `off` was expressible. The design correctly refused. The error
  message therefore names the actual number and the remedy ("raise it above 1024"), because an
  operator will meet this: **a deployment that caps `max_output_tokens` at or below 1024 cannot use
  extended thinking on Anthropic at all.** That is a vendor constraint surfaced honestly, not a
  limitation of this design — the alternatives were to exceed a declared output ceiling (breaking I8's
  budget posture) or to silently downgrade to `off` (breaking 004 §2).
- Clamping means that for small `max_tokens`, `low`/`medium`/`high` can all collapse to the 1024 floor
  — the ordinal is real but the floor dominates. Named here rather than left to be discovered.
- `temperature`/`top_p`/`top_k` remain elided. Reasoning effort was carved out because it has a
  defensible portable shape and an observable effect; those do not yet, and this ADR is not a
  precedent for admitting them.
- Both backends' `build_request_body` gained a defaulted trailing `caps` parameter (OpenAI's did not
  previously take one). Defaulting to an all-false capability set is safe rather than surprising: the
  gate can only fire when a caller *asks* for a level, so no pre-existing call site changes behaviour.
