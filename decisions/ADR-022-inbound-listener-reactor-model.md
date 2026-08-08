# ADR-022 — Inbound listener I/O model: share Quark's reactor, or a standalone one?

**Status:** Judged (2026-08-08). Design B (standalone reactor) accepted. Follow-on to
`decisions/ADR-021-inbound-protocol-trust-boundary.md` §6/§8, which accepted first-party TLS+auth
termination (Design A there) as AgentEngine's strategic direction for MCP/A2A inbound identity, and
explicitly reopened this narrower question rather than deciding it as a drive-by: "does the accepted
design's listener build atop `TcpTransport::event_loop()`, and if not, why is a second from-scratch
reactor implementation justified over the sanctioned extension point the project already has evidence
for?"

## 1. The question

**Does AgentEngine's future inbound HTTP listener (serving MCP Streamable HTTP / A2A JSON-RPC/REST,
per ADR-021) register its LISTENING socket and every accepted connection on Quark's own shared
`pal::IoContext` (obtained via `TcpTransport::event_loop()`, the sanctioned extension point), or does
it run its own, fully independent reactor on its own thread?**

This has a wrong answer in both directions. Get it wrong toward "always build your own" and this
project pays for a second I/O thread/reactor/fd-set per node for no real reason, ignoring
infrastructure ADR-021 §6 already flagged as available and hardened. Get it wrong toward "always
share" without checking whether the SPECIFIC workload fits and this engine's own cluster-membership
traffic (heartbeats, dial/reconnect, the frames Quark's node-to-node protocol depends on for
liveness) shares fate with — and can be starved or delayed by — arbitrary inbound HTTP load, including
adversarial load a hostile MCP/A2A caller fully controls.

## 2. What the scoping research found (2026-08-08)

- **`TcpTransport::event_loop()`** (`third_party/quark/include/quark/net/tcp_transport.hpp:126`)
  returns a live `pal::IoContext&` — the SAME reactor `TcpTransport` uses for its own listener,
  every accepted connection, every outbound dial/reconnect. Its own doc comment: "the ONE sanctioned
  extension point for a sibling seam... do not add any other public surface... for that purpose."
- **`pal::IoContext` is single-threaded by contract.** `add_fd`/`mod_fd`/`del_fd` must be called only
  from the loop thread (or marshaled via the thread-safe `post()`); `run()` owns the loop. This is not
  a suggestion — both PAL backends state it as a banner-level contract.
- **`VoiceChannel` (`third_party/quark/include/quark/net/voice_channel.hpp`) is the ONLY existing
  consumer of `event_loop()`, and its usage shape is materially narrower than what an HTTP listener
  needs**: it registers exactly ONE persistent UDP socket (no `listen`/`accept` at all), via
  `blocking_post()` to respect the loop-thread-only contract. **There is no existing precedent
  anywhere in Quark or AgentEngine for a second component registering its own LISTENING TCP socket —
  one whose accept callback then dynamically registers a growing/shrinking set of per-connection fds
  — on a reactor shared with `TcpTransport`'s own cluster traffic.** Extrapolating from VoiceChannel's
  proven pattern to a listening socket is a real, unproven step, not a confirmed instance of it.
- **`IoContext`'s readiness callback (`ReadyHandler`) is a single combined, level-triggered, per-fd
  callback**, re-armed by the SAME loop thread that dispatches every other registered fd's readiness —
  including, if shared, `TcpTransport`'s own cluster-membership fds. A callback that blocks or runs
  long (parsing a large/adversarial HTTP header set, a slow HMAC verification under load) delays
  every OTHER fd's readiness dispatch on that same thread, by construction of a single-threaded
  level-triggered reactor.
- **No TLS exists at Quark's reactor/transport layer** (`SecureTransport`, ADR-040, is a frame-payload
  AEAD decorator over Quark's own length-prefixed protocol — orthogonal to, and offering nothing
  reusable for, an HTTP/TLS-over-raw-bytes listener). Sharing the reactor buys socket-readiness
  multiplexing only, not any TLS/HTTP capability.

## 3. Competing designs

### Design A — Share Quark's reactor via `event_loop()`

The HTTP listener registers its listening socket and every accepted connection directly on the
`pal::IoContext&` returned by a live `TcpTransport::event_loop()`, following `VoiceChannel`'s own
`blocking_post()`-marshaled registration discipline for the loop-thread-only contract. One I/O thread
per node total, matching ADR-021 §6's own framing of this as "free" reuse.

The load-bearing, previously-unexamined risk this scoping surfaced: this reactor ALREADY carries
Quark's own cluster-membership liveness traffic. Sharing it means an inbound HTTP request — including
one shaped by a hostile MCP/A2A caller, since this is a public-facing protocol surface by definition —
now shares a single-threaded, level-triggered dispatch loop with the heartbeats/dial/reconnect frames
this project's own multi-node operation depends on. A slow or adversarial HTTP callback (oversized
header parse, TLS handshake CPU cost, a deliberately slow-drip connection) does not merely degrade
HTTP service — by construction of a shared single-threaded reactor, it can delay Quark's own
cluster-liveness dispatch on the SAME node, an availability blast-radius connection that did not exist
before this listener shared the thread.

### Design B — A standalone reactor

The HTTP listener owns its own `pal::IoContext` and its own dedicated OS thread (or thread pool),
completely independent of `TcpTransport::event_loop()`. Costs exactly what ADR-021 §6 named: a second
I/O thread/reactor/fd-set per node, and it does not benefit from `TcpTransport`'s own already-hardened
accept-loop code (though `pal::tcp_listen`/`accept_one` themselves — the PAL primitives underneath,
not `TcpTransport`'s own composed logic — are reusable either way; this design does not reinvent the
PAL, only the reactor composition around it).

The genuine advantage this scoping surfaced: HTTP/MCP/A2A traffic is adversarial-by-definition (a
public protocol surface, per ADR-021's own threat framing) in a way Quark's own cluster traffic is
not (cluster membership is a closed, operator-controlled set of peers). Isolating them onto separate
reactors/threads means a hostile or merely slow HTTP client can starve AT WORST the HTTP listener's
own throughput, never Quark's cluster-liveness dispatch on the same node — a strict availability
improvement over Design A's shared-fate risk, not merely a cost/reuse tradeoff.

### Design C — Share the reactor, but only for LISTEN/ACCEPT, hand off each connection

A hybrid: register only the listening socket on the shared `event_loop()` (a cheap, rare, bounded-cost
event — one `accept()` per new connection, not sustained per-connection I/O), then IMMEDIATELY hand
each accepted connection's fd off to a separate, standalone reactor/thread-pool for all subsequent
read/write/parse work. This keeps the shared reactor's exposure to adversarial input bounded to
"accept the fd and hand it off," never to sustained request parsing, HMAC verification, or TLS
handshake CPU cost — while still reusing SOMETHING from the sanctioned extension point rather than
reimplementing the listen/accept step too.

## 4. Falsifiable claims

| # | Design | Claim | Disproving experiment |
|---|---|---|---|
| 1 | A | Registering an HTTP listener + accepted connections on the shared reactor does not introduce measurable added latency to Quark's own cluster-heartbeat dispatch under NORMAL (non-adversarial) HTTP load | Run `TcpTransport` cluster traffic + a shared-reactor HTTP listener under realistic concurrent load; measure heartbeat dispatch latency with and without the HTTP listener attached |
| 2 | A | A single slow/adversarial HTTP connection (e.g. a deliberately slow-drip request, or a callback that runs long) measurably delays Quark's OWN cluster-liveness dispatch on the same node | Inject one deliberately slow/blocking HTTP-listener callback on the shared reactor; measure whether a concurrent cluster heartbeat's dispatch latency spikes in lockstep |
| 3 | B | A standalone reactor fully isolates HTTP-listener load from Quark's cluster dispatch — the same adversarial-callback injection from claim 2 produces NO measurable heartbeat-latency effect | Same injection as claim 2, against the standalone-reactor design |
| 4 | C | The handoff step itself (accept on the shared reactor, immediately transfer the fd to a standalone worker) adds no meaningfully exploitable window where an adversarial accept-time cost (e.g. many rapid connection attempts) can still delay cluster dispatch | Flood connection attempts against the listener; measure heartbeat dispatch latency, isolate whether accept-time cost alone (not subsequent I/O) is enough to cause delay |
| 5 | A, B, C | Whichever design is chosen respects `pal::IoContext`'s loop-thread-only contract for every `add_fd`/`mod_fd`/`del_fd` call — no cross-thread call without `post()` | Code-level audit + a thread-sanitizer or debug-assertion run confirming no direct off-thread mutation of the reactor |

## 5. Red-team brief (next phase)

- Is claim 2's "shared fate" risk real and measurable, or does `IoContext`'s re-arm/dispatch model
  bound any one callback's cost tightly enough that this is theoretical? (Needs the actual measurement,
  not an assumption in either direction.)
- Does Design C's handoff step have its own race/lifetime hazard — is transferring an fd from one
  reactor's registration to another's free of a window where NEITHER reactor is watching it (a dropped
  connection) or BOTH are (a double-dispatch)?
- Given ADR-021's own red-team already flagged DoS shape (connection limits, slowloris, header-size
  ceilings) as unaddressed for the listener regardless of which design wins here — does the REACTOR
  CHOICE itself materially change how hard that follow-on DoS-hardening work is, or is it orthogonal?
- Is "shares fate with cluster liveness" actually a NEW risk this ADR introduces by proposing Design A,
  or does `TcpTransport` already carry this exposure today for its own accepted connections (i.e. is
  Quark's own inter-node traffic already exposed to a slow/adversarial PEER NODE monopolizing the same
  thread, making Design A merely widen an already-accepted risk rather than introduce a new class)?

## 6. Red-team findings

An independent adversarial pass (fresh context) attacked all three designs against real code, not
speculation. Full findings are recorded in the session transcript this ADR was produced in; the
load-bearing ones are captured here.

**The core premise (claim 2) is not merely correct — this codebase already has a measured, executed,
in-repo negative control proving it, and this ADR is entitled to reuse it directly rather than
re-derive it.** `pal::IoContext::run()` has NO per-callback budget, NO worker handoff — `ReadyHandler`
is a plain `std::function` invoked synchronously in-loop; `run_posted()` drains its ENTIRE cross-thread
queue in one unbounded pass before returning to `epoll_wait`; `TcpTransport::do_read()` itself loops
until `would_block` with no iteration cap. `voice_channel.hpp`'s own header comment states directly:
**"ADR-030's F2r negative control proved the unbudgeted variant fully starves the shared loop's other
fds (SWIM/control-plane) at P>=64."** `VoiceChannel`'s entire budget-and-chained-continuation machinery
(`kMaxDatagramsPerWakeup`, `fanout_continuation()`) exists solely as the accepted mitigation for a
failure this project already reproduced. This is exactly the kind of "real, executed evidence" §5's
own bar (decisions/README.md) asks for — reused from ADR-030 rather than re-measured, the same move
ADR-021 made reusing ADR-005's HMAC primitive rather than re-deriving it.

**Design A — sound only under a BINDING implementation constraint, and with a worse failure MODE than
Design B for the same class of bug.** `VoiceChannel` is the existence proof a disciplined consumer
CAN share this reactor safely, but only because its budget/continuation machinery is a load-bearing
invariant, not an optional tuning knob — Design A would need to inherit the identical discipline for
HTTP-shaped work (bounded header/body parsing per wakeup, TLS handshake steps yielded via
continuations) as a hard requirement, not implementer judgment. Separately and more severely: a
loop-thread-only-contract violation in NEW HTTP-listener code (an easy mistake for an implementer
unfamiliar with the constraint) is a data race on `handlers_`/`timers_` — undefined behavior that can
corrupt the WHOLE NODE's cluster transport, not just break the HTTP listener. A real, previously
unflagged, platform-asymmetric hazard: Windows' `add_fd` silently OVERWRITES an existing registration
on double-add (`handlers_[fd] = ...`, always returns `true`); Linux's `epoll_ctl(EPOLL_CTL_ADD, ...)`
rejects it loudly (`EEXIST`). Code that treats `add_fd`'s return value as a double-registration guard
is simply wrong on Windows — this project's own primary dev platform.

**Design B — sound, but its own claimed isolation was overstated and is now narrowed.** Two reactors
genuinely eliminate DISPATCH-LATENCY sharing (the ADR's headline concern) — that much is definitionally
true of using a separate thread and event loop, no new measurement needed. It does NOT eliminate every
shared-fate vector: the OS fd table (`RLIMIT_NOFILE`, process-wide — an HTTP connection flood can still
starve `TcpTransport`'s own `accept`/`dial`/`reconnect` via `EMFILE` even with a perfectly isolated,
idle-fast HTTP reactor), the process heap allocator under memory pressure, and OS scheduler contention
on a core-constrained box. §3's original "strict... never" framing is corrected below to scope the
claim to dispatch-latency isolation specifically, with the remaining vectors named as residuals — this
project already has a directly-applicable mitigation tool for the fd/resource-limit class
(`src/backends/native_jail/job_object_limits.hpp`'s Job-Object-based limits, ADR-004's own precedent),
so this residual is tooled, not unsolved-from-scratch, when the listener is actually built.

**Design C — rejected: sound in concept, but strictly dominated by Design B for this workload, with a
real unaddressed hazard.** Its entire safety property rests on the shared-reactor accept handler doing
LITERALLY ZERO byte-level work — a discipline nothing in `IoContext`'s API enforces and a future
maintainer (adding ALPN/protocol sniffing "temporarily") can easily violate, reopening exactly the
adversarial-content exposure Design C exists to avoid. It has a genuine, currently-unaddressed fd-leak
hazard: between `accept()` succeeding on the shared thread and the `post()`-marshaled hand-off actually
completing on the standalone reactor, the fd is watched by NEITHER reactor — and if anything fails
during that window (an exception, an allocation failure), nothing ever closes it. And it does NOT
resolve pure connection-VOLUME flooding of the shared thread (`on_accept_ready()`'s own unbounded
per-invocation backlog drain still runs there) — exactly what this ADR's own §4 claim 4 already
flagged as unmeasured, now confirmed as a real, not merely theoretical, residual. Design C buys
"slightly cheaper than Design B" at the cost of a real correctness hazard and an incomplete fix for the
very problem it exists to solve; not worth it against Design B's small, bounded, one-extra-thread cost.

**A finding outside this ADR's own scope, recorded rather than lost:** `TcpTransport::on_accept_ready()`
accepts ANY TCP connection with zero authentication before allocating state and registering it on the
shared reactor — `parse_hello()` validates only a magic number, a version, and a self-reported,
UNAUTHENTICATED `NodeId`. This means the starvation attack this ADR analyzes for HTTP traffic is
ALREADY possible against Quark's own cluster port today, by anyone with network reachability to it —
gated only by deployment/firewall topology, not by anything the code enforces. This is a real,
pre-existing gap in Quark itself, not something this ADR fixes (CLAUDE.md: "Quark is a submodule,
never forked or patched in-tree — runtime changes go upstream") — named here so it is not lost, and
flagged as worth raising upstream. It also reframes what Design A actually changes: not "introducing a
threat model to a previously-safe component" (the component was never code-safe against this, only
protected by network placement), but "escalating the POPULATION with a lever on an already-unprotected
mechanism" from cluster-adjacent peers to any internet-facing MCP/A2A caller — still a real, meaningful
escalation, just a different one than "creating a new hole."

## 7. Decision

**Design B (a standalone reactor/thread for the inbound HTTP listener, fully independent of
`TcpTransport::event_loop()`) is accepted.** Rationale, weighed directly against Design A now that
both are understood precisely:

- Design A's safety is CONDITIONAL on discipline (VoiceChannel-shaped budgeting) that must be
  correctly implemented once and never regressed by any future contributor touching HTTP-listener
  code, forever. Design B's isolation is structural — a bug in the HTTP reactor's own dispatch cannot,
  by construction, corrupt `TcpTransport`'s own maps or starve cluster-liveness dispatch. This project
  has an established preference for "unforgeable/impossible by construction" over "correct by
  convention" wherever the cost is reasonable (ADR-009's own capability-set design is the clearest
  precedent) — Design B is the construction-level guarantee, Design A is the convention-level one, for
  a workload (adversarial, internet-facing, security-critical) where getting it wrong is expensive.
- Design A's failure MODE is worse for the identical class of implementer mistake: a loop-thread
  violation corrupts the whole node's transport state under Design A, and only the HTTP reactor's own
  state under Design B. Blast radius, not just probability, favors B.
- Design B's cost — one additional OS thread/reactor per node — is small and bounded, not remotely
  prohibitive for a project already running Quark's own multi-threaded engine. The overclaimed-isolation
  residuals found above (fd table, heap, CPU scheduler) are real but apply in some form regardless of
  which design is chosen, and this project already has a directly-applicable mitigation tool
  (Job-Object-based limits, ADR-004) for the sharpest of them (fd/resource exhaustion) when the
  listener is actually built.
- Design C is rejected outright: it does not clearly beat Design B on any axis strong enough to
  justify its added complexity and its own real, currently-unaddressed fd-leak hazard, and it does not
  actually solve the volumetric-flooding exposure it was proposed to narrow.

**Evidence basis**: this ADR is Judged on real, executed evidence — reused directly from ADR-030's own
F2r negative control (VoiceChannel's own header comment cites it), the same "cite proven evidence
rather than re-derive it" move ADR-021 made reusing ADR-005's HMAC primitive — plus direct code-level
analysis of `IoContext`/`TcpTransport`/`VoiceChannel` with file:line citations. No new measured
benchmark code was written for this pass: there is no listener yet to attach one to, and the specific
mechanism in question (a single-threaded, unbounded-per-callback reactor) is unchanged between
`VoiceChannel`'s already-measured context and this one, so re-measuring it here would reproduce a
result this codebase already has rather than test anything new.

**What this ADR does NOT decide, left as follow-on work for whenever the listener is actually built**:
the standalone reactor's own thread-pool sizing/backpressure shape; the fd/resource-limit mitigation
(Job-Object reuse) named above; server-role TLS and HTTP/1.1 request parsing (ADR-021 §7's own named
residuals, unaffected by this decision); and — separately, outside this project's own authority —
raising the unauthenticated-`parse_hello()` finding with Quark upstream.
