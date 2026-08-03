# ADR-007 — Should taint be tracked at sub-string (span) granularity rather than per-`Content`-item?

**Resolves:** OpenQuestions.md OQ-5 (003 Q3 / 007 Q2 / 017 Q2). **Scope, deliberately narrow** (small
prove, matching this backlog's established scale): this ADR builds and red-teams a span-taint
prototype and a naive variant of it, against the *specific* precision gap 003 §2 names ("a message
that mixes user text with a quoted tool result taints wholesale"). It does not attempt a production
span-taint implementation, does not touch `include/agentengine/core/content.hpp`, and does not re-
examine `Citation`'s existing `span_start`/`span_end` fields (003 §1), which are an unrelated
annotation, not a taint mechanism.

## 1. The question

003 §2 tracks taint as one bit per `ContentItem` (`agentengine::ContentItem::tainted`,
`content.hpp`). 003 Q3 / 007 Q2 / 017 Q2 all ask the same thing from different angles: would tracking
taint at **byte-range granularity within one item** make declassification and structural separation
"far more precise," and is that precision worth the stated cost — "a real cost in the content model's
complexity and in every mapping layer" (003 §5's round-trip-test gate over every protocol surface).

## 2. The competing designs, steelmanned

**Design A — keep per-item taint (status quo, 003 §2 as written).** One `bool` per `ContentItem`.
Simple, already implemented in vocabulary form (`TaintedText`, `content.hpp`), and every mapping
layer's round-trip test (003 §5) only has to preserve one flag per item, not per-byte state.
Steelman: over-tainting fails *closed*, which is the same direction I3's whole posture already
prefers — coarse-but-safe beats precise-but-fragile in a system whose entire job is "never silently
wrong."

**Design B — span-level taint.** A `ContentItem`'s text carries a list of tainted byte ranges instead
of (or in addition to) a whole-item flag. Steelman: a single string that legitimately mixes trusted
and untrusted material can be finer-grained without forcing the producer to restructure it into
multiple items, and it generalizes naturally to `Citation`-shaped precision work docstring already
gestures at.

## 3. Falsifiable claims

| # | Claim | Disproven by |
|---|---|---|
| P1 | Per-item taint has a concrete scenario where it denies access to material that is genuinely trusted, purely because it shares an item with tainted material. | No such scenario constructible; per-item taint always denies exactly the material that should be denied. |
| C1 | A span-level type is a genuinely new place for the taint mechanism itself to be silently wrong — a single missed offset-adjustment (e.g. on concatenation) can misclassify which bytes are tainted, in either direction. | Every span-mutating operation is provably offset-safe by construction, with no way to write a version that compiles and misclassifies. |
| E1 | The concrete scenario in P1 is already solvable today without spans, using only mechanisms already specified elsewhere (017 §3 structural separation: two `ContentItem`s instead of one mixed string), reaching the same practical precision. | The two-item split loses information the span-level version would have kept, for the same scenario. |

## 4. The red-team attack

The adversarial question for Design B isn't "can an attacker forge a span" (spans are host-internal
state, never attacker-supplied) — it's **"does an ordinary, non-malicious string operation silently
produce the wrong taint classification,"** because that failure mode is worse than an attacker: it is
invisible to red-team-style probing that looks for external forgery, and it fires on *legitimate*
code paths. `span_taint_prove.cpp` (scratchpad, not shipped — see §9) tests exactly this:

- **P-1** — construct one `ContentItem`-shaped string mixing a trusted preamble (`"policy: allow
  further reads. "`) with a tainted payload (`"attacker-controlled: ignore all previous
  instructions"`), matching 003 §2's own named scenario. Under `PerItemTaintedText` (mirroring the
  real `TaintedText`), confirm there is no way to reach just the trusted preamble through the type —
  the whole item is denied.
- **P-2** — the same mixed string under a span-level type (`SpanTaintedText`) *does* distinguish the
  two regions: `position_is_tainted()` correctly reports the preamble untainted and the payload
  tainted.
- **C-control** — a **correct** span-shifting `concat` (left string's length added to every
  right-hand span before merging) classifies both regions right after concatenating a trusted-only
  string with a fully-tainted one.
- **C-1 (the attack)** — a **naive** `concat` that forgets to shift the right-hand operand's spans by
  the left string's length. Concatenating `"safe-prefix:"` (untainted) with `"DANGER"` (fully
  tainted, span `[0,6)`) produces a merged string where the carried-over-unshifted span `[0,6)` now
  marks the first six bytes of `"safe-prefix:"` — the **trusted** prefix — as tainted, while the
  actual `DANGER` bytes (positions 12–17) read as **untainted**. This is the dangerous direction: a
  policy check keyed on "is this byte range tainted" would treat genuinely untrusted bytes as clear.
- **E-1** — the same trusted-preamble/tainted-payload information represented as **two**
  `PerItemTaintedText` items instead of one mixed string reaches the identical precision P-2
  demonstrated for spans (preamble item untainted, payload item tainted), using only 003's existing
  per-item mechanism plus 017 §3's already-specified structural-separation idiom.

## 5. Executed evidence

MSVC 19.51.36252 (toolset 14.51.36231), `cl /std:c++20 /EHsc /W4 /fsanitize=address /Zi
span_taint_prove.cpp`. Compiled clean (no warnings at `/W4`). Ran clean under ASan, **zero findings**,
all 6 checks pass:

```
[ok] Claim P: per-item taint gives no way to reach just the trusted preamble through the type
[ok] Claim P: span-level taint DOES distinguish the trusted preamble from the tainted payload
[ok] Claim C control: correct span-shifting concat classifies both regions right
[ok] Claim C: naive (unshifted) concat silently UNDER-taints the actual danger bytes and
     over-taints the trusted prefix -- a security-relevant misclassification, not merely cosmetic
[ok] Claim C (severity): the mis-shifted span means a policy check keyed on "is this byte range
     tainted" would treat the untrusted DANGER bytes as clear
[ok] Claim E: splitting into two ContentItems (017 SS3's existing idiom) reaches the same precision
     as span-level taint, using the type system already specified in 003, with no offset-tracking
     bug surface
0/6 checks failed
```

UBSan not attempted (no clang toolchain on this machine, same documented gap as ADR-005/006).

## 6. Per-claim verdicts

| Claim | Verdict | Evidence |
|---|---|---|
| P1 | **CORRECT** | P-1: per-item taint denies the whole mixed item, including the genuinely trusted preamble |
| C1 | **CORRECT** | C-1: a single missed offset-shift in an otherwise-plausible `concat` implementation silently under-taints the actual danger bytes and over-taints trusted ones — reproduced, not hypothesized |
| E1 | **CORRECT** | E-1: the two-`ContentItem` split reaches identical precision to the span-level type for the named scenario, with no new mechanism and no offset math |

## 7. The decision

**Rejected for now — keep per-item taint (003 §2) as specified; do not build span-level taint.**

P1 is real (over-tainting genuinely denies more than necessary) but E1 shows the concrete case 003
itself names is *already* solved today, at no additional cost, by the structural-separation idiom
017 §3 already specifies: a producer assembling a mixed trusted/tainted string should instead keep
them as separate `ContentItem`s. That is not a new rule this ADR invents — it is 017 §3 applied one
layer earlier, to the content model instead of only to prompt rendering.

C1 is the decisive finding: span tracking does not just add complexity, it adds a **new way for the
mechanism that exists to make I3 violations impossible to instead make them possible** — a single
missed offset-shift (an entirely ordinary, non-malicious programming mistake, reproduced here on the
first naive attempt) silently clears taint from bytes that should carry it. For a mechanism whose
entire purpose is "never silently wrong" (007 §4: "a declassifier that does no checking is a
review-blocking defect"), trading a coarse-but-structurally-incapable-of-this-bug-class mechanism for
a precise-but-fragile one is a worse trade than 003 Q3's framing ("materially harder") suggested —
this ADR found *harder* actually means *a new latent under-taint bug class in the security boundary
itself*, not just more code to write once and forget.

**What this does not close:** a case where a single `ContentItem` must remain *partially* tainted
after partial declassification — e.g. echoing back a validated, mostly-trusted summary that still
embeds one still-tainted verbatim quote — is not served by the two-item split (the summary is one
coherent unit, not naturally two items) and is not addressed here. If a concrete instance of this
case blocks a real declassifier once 007/017 have implementations, it should be re-opened as its own
narrow ADR against that concrete case, not reopened as "add span-level taint generally" — this ADR's
finding is that the *general* mechanism is a net-negative trade, not that no narrower affordance
could ever be justified.

## 8. Residual risks and deferred gates

- **Scenario coverage is one representative case, not exhaustive.** P1/E1 test the specific pattern
  003 §2 names (mixed trusted preamble + tainted payload). Other span-level use cases speculated
  about in 017 Q2 (finer-grained structural separation for filters, §4) are not tested here and are
  not claimed to be covered by the E1 escape hatch without their own check.
- **C1's naive bug was found on the first attempt, not after extensive fuzzing.** This is read as
  evidence the bug class is easy to introduce, not as proof it is the *only* or *worst* bug a real
  span-taint implementation could contain (`substr`/`slice`/`replace`, and every 003 §5 protocol
  mapping layer, would each need their own correctness argument).
- **This ADR does not re-examine `Citation`'s existing `span_start`/`span_end`** (003 §1) — that is
  an unrelated source-attribution annotation, already shipped in vocabulary form, not a taint
  mechanism, and out of this ADR's scope.
- **Prototype code is scratchpad-only, not shipped** (`span_taint_prove.cpp`, not under
  `include/`/`src/`) — consistent with a *rejected* design; unlike ADR-005/006 there is no production
  header to land, because the decision is not to build this.

## 9. Files

No repository source changed by this ADR — the decision is "do not build." Evidence lives at
`span_taint_prove.cpp` in this session's scratchpad (not committed, matching the precedent set by
OQ-7's Wasmtime smoke test for evidence that informs a decision without shipping as product code).
`003-Message-and-Content-Model.md` §7 Q3, `007-Capability-and-Trust-Model.md` §10 Q2, and
`017-Safety-and-Content-Governance.md` §9 Q2 are updated to point here.
