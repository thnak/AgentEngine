# Decisions — the ADR record

An **ADR** here is not a memo. It is the durable record of a `design → red-team → prove → judge`
loop: competing designs implemented in real C++23, attacked adversarially, compiled under multiple
compilers, run under sanitizers, and measured — before a judge picks a winner on evidence.

This is the process Quark uses (36 ADRs and counting), and it exists because the alternative —
settling a hot-path or security-critical question by argument — produces designs that read well and
fail under load or attack.

## When an ADR is required

- Any **security-critical** choice: capability representation, isolation boundary, taint mechanism,
  approval binding, secret handling.
- Any **hot-path** choice measured by a 023 budget.
- Any choice where **credible designs disagree** and the disagreement is not resolvable by reading.
- Any **promotion** of an RFC from Draft to Proven — the gate execution *is* the ADR.

Ordinary implementation choices do not need one. An ADR for everything is an ADR for nothing.

## What an ADR must contain

1. **The question**, stated so that it has a wrong answer.
2. **The competing designs**, each steelmanned.
3. **Falsifiable claims** per design — specifically the fast/safe/correct claims, each paired with
   an experiment that would *disprove* it.
4. **The red-team attack** on each design, hardest where it claims to be fast or safe.
5. **The executed evidence**: commands, compilers, sanitizers, measured numbers (percentiles, not
   means), with **positive controls** — a test that cannot fail proves nothing, and for security
   claims this is mandatory (022 §5).
6. **Per-claim verdicts**: CORRECT / WRONG / INCONCLUSIVE, decided by observed output, not argument.
   `INCONCLUSIVE` is an honest verdict and must not be laundered into either of the others.
7. **The decision**, the specs it binds, and the residual risks or deferred gates.

## Naming

`ADR-NNN-<kebab-case-question>.md`, numbered sequentially and permanently. A superseded ADR is
marked Superseded and points forward; it is never deleted, because the reasoning that was wrong is
part of the record.

## Index

*(empty — the project is in its design phase; the first ADRs will come from the promotion gates
named in the RFCs, and from the questions in [`../OpenQuestions.md`](../OpenQuestions.md))*

| ADR | Question | Outcome |
|---|---|---|
| — | — | — |
