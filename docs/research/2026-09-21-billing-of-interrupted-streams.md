# Do providers bill a stream that never finished? (ADR-177 §4, I8)

- **Date**: 2026-09-21
- **Question**: when a streamed model response dies mid-answer and the session retries, did the provider
  bill the dead attempt? It decides whether ADR-177's retry can silently spend money the per-run token
  budget never sees.
- **Answer: NOT ESTABLISHED. No first-party statement was found.** Treat a discarded attempt as
  *possibly billed*.

## What was checked, and what it said

| Source | Kind | Finding |
|---|---|---|
| DeepSeek API pricing page, <https://api-docs.deepseek.com/quick_start/pricing> | first-party | States the cost formula ("number of tokens x price"). **Does not address interrupted, cancelled or incomplete streams.** Fetched 2026-09-21. |
| OpenAI Developer Community, "If we stop streaming output stream before it finishes, do we still get billed...", <https://community.openai.com/t/if-we-stop-streaming-output-stream-before-it-finishes-do-we-still-get-billed-for-the-tokens-that-werent-ouputted/859904> | community forum, question dated 2024-07-09 | **The question, unanswered** in the fetched content: no OpenAI staff reply, no community answer. Evidence of the question only. |
| BerriAI/litellm PR #39893, <https://github.com/BerriAI/litellm/pull/39893> | third-party proxy | Says its OWN cost log used a placeholder output token for an interrupted Anthropic stream and now uses the recovered count. **Cites no Anthropic documentation and no measurement** of what Anthropic bills. |
| Search-result summaries (Mistral course page, Flexprice, forum threads) | secondary | Assert "you pay for tokens already generated". Not first-party; not verified. |

## What this does and does not support

- It does **not** support "a discarded attempt is free". Nothing found says that.
- It does **not** support "a discarded attempt is billed in full" either. The plausible reading (input
  billed once the upstream starts answering, output billed up to the cut) comes from secondary sources
  and was not verified.
- Providers differ, and each one's answer can change. A claim about one belongs in a dated note like
  this, not in code.

## Consequence for the design

**Decision (project owner, 2026-09-21): since no provider is clear about it, believe they still count a
discarded attempt toward cost.** ADR-177 therefore CHARGES it to the per-run token budget as an estimate
(the request as input plus the output the dead stream delivered, ~4 bytes/token, rounded up), never into the
real-usage report, and refuses to retry once that charge breaks the budget. The count is also bounded
(per-run cap, clamped to 3), announced (`ModelOutputDiscarded`, carrying the estimate) and exposed
(`stream_retries_used()`, `discarded_tokens_estimate()`).

The estimate is a bias toward over-counting, chosen because the alternative -- treating it as free -- has
no evidence behind it either and errs the unsafe way for a budget.

To replace the assumption with a fact someone needs a first-party statement per provider, or a
measurement: make a streamed call, kill it at a known point, and compare the provider's usage dashboard
with the tokens actually received. That needs a provider account and is not done here.
