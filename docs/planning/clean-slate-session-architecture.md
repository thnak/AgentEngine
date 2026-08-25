# Clean-slate session architecture — four layers, stated once, no legacy constraints

**Status: foundational model only. Deliberately ignores every existing mechanism (`ContextProvider`
fan-out, `turn_middleware` chain, `ADR-066`/`067`/`074`/`096`, `OQ-18`) as a starting constraint —
per explicit instruction: reconciling with what already exists, continuously, is what has kept this
from converging. This document states the target shape first. Mapping it onto real code, and what
that displaces, is separate, later work — not attempted here.**

## The four layers

**1. Worktree — the only place state durably lives.**
Content-addressed (`Blob`/`Tree`/`Ref`). Branch is copy-on-write. Checkpoint is a commit. Nothing
about *how* a session runs lives here — this layer is storage and versioning, full stop. It has no
opinion about sandboxes, chat clients, or models.

**2. Sandbox — the execution environment.**
Given a worktree location (a Ref, or a subtree of one), a Sandbox materializes a real place code can
run and executes against it. Its own output (files written, state changed) is not itself durable —
durability is the worktree's job, one layer down. A Sandbox reads FROM the worktree and writes BACK
to it; it does not own persistence semantics itself, only execution.

**3. ContextProvider — prepares what the chat client sees.**
The chat client's own interface is small and fixed: messages, tools, a handful of small config
values. Everything else in a session — worktree content, sandbox state, skills, memory, policy — is
richer than that, and none of it reaches the chat client directly. `ContextProvider`'s job is the
translation: enrichment (pull relevant material in) and filtering (cut what shouldn't reach the
model) on the way there, and the same on the way back. Two call surfaces, not one:
- **pre** — before the model is called: build and shape what goes in.
- **post** — after the model responds: process what comes out, before it's treated as final.

Both surfaces belong to the SAME concept — a `ContextProvider` is something that runs at defined
points around a model call, not two unrelated mechanisms (today's split between fan-out and a
separate turn-middleware chain is exactly the kind of accumulated-not-designed shape this document
is stepping back from).

**4. Middleware — not addressed here.**
Deliberately out of scope for this pass. Whatever relationship it has to layer 3's pre/post surfaces
is a real open question, named, not answered.

## What each layer does NOT do (the boundaries, stated explicitly)

- Worktree does not execute anything and does not know what a "session" or a "model" is.
- Sandbox does not decide what the model sees and does not itself persist anything durably.
- ContextProvider does not execute code and does not store anything durably — it reads from
  whatever layers 1/2 already have and shapes it into the chat client's minimal interface.
- The chat client never sees a worktree, a sandbox, or a capability — only messages/tools/config.

## Open questions, named rather than guessed at

- What exactly is on `ContextProvider`'s `pre`/`post` surfaces — one call each, or several (today's
  accumulated mechanisms suggest `pre` alone might still want sub-phases: independent enrichment vs.
  ordered filtering — is that two surfaces or one with internal structure)?
- How does a `ContextProvider` reach a session's `Sandbox`/worktree state — direct reference,
  something passed at each call, something else?
- Where does Middleware (layer 4) fit relative to layer 3's `pre`/`post` — same mechanism, a
  different one that wraps it, something else?
- How do `fork_from()`/`agent.spawn`'s branch-and-checkpoint operations (layer 1) get triggered —
  do they belong to the session itself, to layer 2, or to a caller sitting above all four layers?
- Nothing here yet says how many `ContextProvider`s can exist per session, how they compose with
  each other (if at all), or what "declared order" even means once there are two surfaces instead
  of one merge point.

This document stops here deliberately — the four-layer split and the boundary statements are the
thing to agree on first. Everything past that (exact signatures, how many providers, how composition
works) is next.
