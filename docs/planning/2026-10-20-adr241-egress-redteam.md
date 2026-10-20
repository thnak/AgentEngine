# ADR-241 red-team record (rounds 1-4, 2026-10-20)

The adversarial reviews of `decisions/ADR-241-egress-proxy-async-path-on-rt-http.md`, kept verbatim as the ADR's evidence. Round 1: 0 FATAL / 8 MAJOR / 8 MINOR; round 2: 0 / 2 / 8; round 3: 0 / 2 / 6 (S1 cleared for the prove step; S2's blockers resolved in the round-4 revision); round 4 (S2 only): 0 / 2 / 4, resolved in the round-5 revision. The reviews read code only; nothing was built.

## ADR-241 red-team, round 1

Result: 0 FATAL, 8 MAJOR, 8 MINOR. The SSRF core (resolve once, filter by address family, connect to the verified address, TLS checked against the name) holds up. Four kinds of problem need design changes before code:
- budget claims that are false (findings 1 and 7);
- a wrong error and class table (finding 2);
- runtime lifetime and stop-token plumbing (findings 3, 4 and 5);
- a proof plan whose named mutants survive (findings 6 and 8).

### MAJOR

#### 1. The "worst case = effective_cap + 64 KiB" claim (D4) is false. Interim 1xx responses have no bound.
- **Where:** `rt/http.hpp` open_stream, step 6 (about lines 735-750).
- **What happens:** on every 1xx response except 101, the code runs `parser = http_wire::HeadParser(ctx.options.limits)` and `continue`. That is a fresh 64 KiB head budget each time, and no counter limits how many interim heads can arrive.
- **Attack:** an allowlisted (or compromised) peer streams `HTTP/1.1 100 Continue\r\n\r\n` without stopping. None of those bytes count against `max_body_bytes` or the grant's `byte_cap`. Only `total_timeout` stops it: 120 s at link speed. The sync path took the first head as final, so its head+body total was bounded by the cap.
- **Two smaller gaps in the same claim:**
  - Trailers get another head-cap allowance ("trailers count too" against the head cap), so the real worst case is at least cap + 64 KiB + 64 KiB + unbounded 1xx.
  - A grant with a tiny `byte_cap` (say 1 KiB) still admits a 64 KiB head. The "rejected alternative" section misses the simple option: `max_header_bytes = min(64 KiB, effective_cap)`. That check runs while reading, so it is not post-hoc.
- **Required:**
  - Add a request-level wire-byte counter in `rt::http`. It counts every head, interim head, body and trailer byte against one total: `effective_cap + min(64 KiB, effective_cap)`. Alternatively, cap interim responses at a small number such as 8 **and** count their bytes.
  - Narrow `max_header_bytes` by the cap.
  - G9 must add a 1xx flood row and a "head > tiny cap" row. The mutant is "reset the parser without counting".

#### 2. The error table (section 9) gets today's TLS codes and classes wrong; the planned mapping would downgrade a certificate rejection to `transient`.
- **What the code does today:**
  - The sync TLS handshake does not produce `net.connect_failed`. It goes `TlsClientSession::handshake` (`src/sandbox/tls_client.cpp:122`) to `mbedtls_client::handshake_error` (`src/backends/tls_mbedtls/mbedtls_client_config.cpp:101-110`).
  - That returns `net.tls_certificate_rejected` or `net.tls_handshake_failed`, both **`policy`**.
  - Configuration failure is `net.tls_setup_failed`, **`fatal`** (`:66-99`).
- **What the ADR plans:** the table row "(TLS failure was net.connect_failed) transient -> net.tls_handshake_failed transient". A local `egress_error` that maps `http_error::tls_failed` to `transient` would turn a MITM or wrong certificate into something retry sites retry, and would turn a broken CA bundle (`fatal`) into an endless transient.
- **Knock-on effect on G11:** it expects `net.tls_handshake_failed` for a name mismatch. The real code is `net.tls_certificate_rejected`, so the test is pinned to the wrong value.
- **Rows missing from the table:**
  - `net.stream_read_failed` (`http.hpp:201`, `http_tls.hpp:92`);
  - `net.too_many_open_files` (`http.hpp:201`; this one is resource-class);
  - `net.consumer_stalled`;
  - `net.tls_setup_failed`.
- **Required:**
  - Map by `code`, not by `http_error`, for `tls_failed`: `tls_certificate_rejected` and `tls_handshake_failed` become `policy`, `tls_setup_failed` becomes `fatal`. Better still, carry the TLS client's `failure_class` through `HttpTlsHandshake`.
  - Correct every row in the table.
  - G12 must produce a certificate rejection through **both** paths and assert the class is identical (`policy`).

#### 3. The stop token omits Runtime shutdown and scope cancellation, and the per-call deadline is ignored.
- **What D1 does:** it threads only `ctx.cancellation`. The established precedent for every `rt::http` consumer merges in `rt::scope_stop_token()`:
  - `src/protocol/sse_stream_pump.hpp:203-205`;
  - `core/replay_chat_client.hpp:151-153`.
- **Why that matters:** `Runtime::~Runtime` requests stop on the **root** token (`rt/runtime.hpp:430-437`), and a `when_all` sibling failure requests stop on the **scope** token. Neither reaches `ctx.cancellation`.
- **Result:**
  - A parked fetch holds up host shutdown for up to `total_timeout` (120 s), plus any queued DNS.
  - A failed sibling in a parallel tool batch does not cancel the fetch.
  - `EffectContext::deadline` (`core/effect_context.hpp:55`, the per-call timeout hint) is ignored. A tool call with a 10 s deadline can hold its lane's work for 120 s.
- **Required:**
  - `HostEgressProxy::fetch` builds a merged `stop_source` (caller token + `rt::scope_stop_token()`), as `sse_stream_pump` does.
  - The effective `total_timeout` becomes `min(total_timeout, ctx.deadline - now)` when a deadline is set. The caller passes it in, still as a value, never as `EffectContext`.
  - G7 must add a "destroy the host Runtime while a fetch is parked (it returns `canceled` well under 1 s)" row, and a `when_all` sibling-failure row.

#### 4. The D8 private-runtime lifetime moves a blocking thread join onto a host lane.
- **The claim:** "the last release happens on the caller's home, never on a runtime thread, so `~Runtime`'s join cannot self-join". That is true, and it is the wrong hazard. The caller's home is a **host lane**.
- **What `~Runtime` does there** (`rt/runtime.hpp:450-453`):
  - `dns_.shutdown()` joins running bodies, meaning a `getaddrinfo` still in flight (R1: glibc about 30 s, unbounded in principle);
  - `lanes_.stop()` joins;
  - `reactor_.reset()` joins.
- **The exact sequence that triggers it:**
  1. The user cancels during slow DNS.
  2. The tool returns `canceled`.
  3. The session and its ToolTable are torn down on its strand.
  4. The last holder reference drops, and that lane blocks for the rest of the lookup.
- **Related problems:**
  - "Per proxy family" means one runtime per tool *class*: 5 tools give 5 runtimes, about 25 threads, each with an unused lane.
  - `wasm_backend.cpp:566`, every existing test, and the current `invoke()` bodies still construct `HostEgressProxy{}` per call. Nothing in the design stops a future caller from doing the same, which means one runtime plus a join per fetch.
  - A ToolTable with static storage duration runs these joins during static destruction.
  - Runtime construction failure (`std::system_error` from thread spawn) inside the coroutine has no defined mapping.
- **Required:**
  - Replace per-family holders with **one process-wide egress I/O context**: a reactor plus a dedicated DNS pool. It is not a `Runtime`; lanes are not needed.
  - Create it once and never destroy it from engine threads: either intentionally leak it, or tear it down through an explicit host shutdown hook that runs off-lane.
  - Map a construction failure to a `resource` error.
  - G13 must add a timing gate: destroy the frame while a fake resolver sleeps 5 s, and assert the destroying thread returns in under 100 ms. The current design fails this; that is the mutant.

#### 5. A borrowed `Runtime*` puts guest egress DNS in the provider plane's DNS pool.
- **What D8 does:** it takes `Runtime::dns_pool()` from a borrowed runtime. The provider clients (ADR-237 steps 43-57) use that same pool for model-call DNS.
- **Effect:** egress lookups to a slow allowlisted name (an outage, or a host whose authoritative DNS stalls) occupy those workers. That stalls the agent's own **model calls** across every session.
- **Why R1 understates it:** R1 frames this as "egress starves egress". It is cross-plane and cross-session, and the sync path never had the coupling (it resolved on the calling thread).
- **Required:**
  - Egress always uses its own DNS pool, even with a borrowed runtime; borrow the reactor only.
  - Add a per-session in-flight lookup cap, for example an `AsyncQuota` of 1-2. Running out of it is `resource`.
  - Name the cross-plane property in a gate: saturate egress DNS and show a provider-pool lookup still completes.

#### 6. "One seam pair" is still two implementations of resolve and filter, and the sync half has a v6-to-v4 truncation hazard.
- **What section 4 says:** `fetch_blocking` "calls `host_resolver`, filters through `address_policy`, picks the first accepted v4 address, builds a `VerifiedEndpoint`".
- **What `rt::resolve`/`connect_resolved` does that this sentence does not specify:**
  - **Numeric skip** via strict `pal::parse_ip_address`, so the resolver is not called (`rt/dns.hpp:228-232`). If the sync path calls `host_resolver` for a numeric host, G1's "resolver count == 1 per passing row on both paths" diverges, and a fake behaves differently per path.
  - **Catching a throwing policy** (`:271-275`).
  - `valid_host` (`:233`).
- **The truncation hazard:** `VerifiedEndpoint` is a `uint32`. If any non-default `address_policy` accepts a v6 address (the field is public, and a deployer wanting v6 will set it), the sync path builds an endpoint from `bytes[0..3]` of a v6 address and connects to an address the policy never saw.
- **Required, one of two:**
  - Specify exactly one shared synchronous function (numeric skip, `valid_host`, try/catch policy) and have the sync path **skip non-v4 structurally**, whatever the policy says. Add a G4 row: permit-all policy plus a v6 answer on `fetch_blocking` gives `address_blocked` or unresolvable, never a connect.
  - Better, and preferred: make `fetch_blocking` drive the async `fetch` to completion on the egress context from finding 4. This removes R3 and R4 entirely: same parser, same byte-cap meaning, same deadline, same Host header. First check whether the WASM import thread is a lane or an offload worker; `Runtime::run` and `block_on` refuse those (`runtime.hpp` header comment), so a plain completion signal (future/latch) would be needed.

#### 7. The sync-edge deadline (section 7) is false for HTTPS and for DNS. G8's sync half is not feasible as written.
- **HTTPS:** `TlsClientSession::recv` (`tls_client.cpp:143-151`) loops inside `mbedtls_ssl_read` until a whole record arrives. Each `bio_recv` waits up to `io_timeout_ms`. A peer that sends one byte every 89 s of a 16 KiB record never returns to the top-of-loop check. The handshake has the same shape.
- **DNS:** the sync path's `getaddrinfo` is blocking and has no bound.
- **So:** "Bound: total_timeout + one idle wait" holds only for plain-HTTP reads. R3's "bounded ... after the scope-added deadline check" is wrong.
- **G8 sync half:** it needs `io_timeout_ms` "overridden small", but that value is read once into a function-local static (`sandbox/io_timeout.hpp:14,28-29`). It cannot be changed in-process, and overriding it for the whole test binary changes every other parametrised row.
- **Required:**
  - Either fix finding 6 by running the async path, or put a deadline check inside the TLS BIO (`bio_recv`/`bio_send` compute their remaining wait from the overall deadline).
  - State that DNS is unbounded on the sync edge.
  - Make the idle bound a parameter of the exchange functions (not the static) so G8 can set it.

#### 8. G1 cannot kill its own named mutants; the proof that the gate is shared is weaker than stated.
- **The mutant "async path omits `reject_crlf`":** `rt::http`'s `open_stream` step 1 runs `http_wire::build_request` before any resolve (`http.hpp:655-660`). It rejects CR, LF and NUL with the **same** code, `net.header_injection_rejected`, and the resolver count stays 0. So G1 passes with the mutant applied.
- **C3 https-without-TLS:** same problem. `ctx.tls` empty gives `tls_unavailable` with the same code `net.scheme_unsupported` and zero resolves (`:670-675`).
- **G2's async half:** proves `rt::http`, not `egress_gate`.
- **What actually discriminates:** only C1 (zero or two entries), the malformed entry, and C7 (method restriction).
- **Required:**
  - Name the mutants that G1 really kills: C1, C7, and malformed-entry omitted on async.
  - For shared-gate identity, add a counting or spy hook on `egress_gate` (test-only), or a row that only `egress_gate` rejects for each claim.
  - State plainly that CRLF/NUL rejection on async is defence in depth from two layers. "Neither load-bearing alone" then has to be proven by mutating each layer separately and seeing the other one hold.

### MINOR

9. **Guest-visible compatibility break that the ADR does not flag.**
   - WASM guests whose HTTP library sets `Content-Length` or `Host` (common) now fail on every request, and get `host-not-allowlisted`, which is misleading and hard to debug.
   - Add this to section 9's "Flagged changes". Decide between stripping and recomputing the four managed headers (safe, because the proxy owns framing) and rejecting them. If you reject, map `net.invalid_request` to `network-error` or document it in `wit/ae-tool.wit`.
   - Likewise for `read_content`/`extract_pdf_*`: a model URL with a space or control byte becomes a `contract` error (previously sent raw).

10. **`egress_gate` runs the full `build_request`, copying the whole guest body** (`http_wire.hpp:247-258`) just to validate. That doubles memory for large WASM bodies. Split out a validate-only function and have `build_request` call it.

11. **New production trust-root surface.**
    - `tls_ca_override` as a public field on `HostEgressProxy` makes CA replacement reachable from any holder of a production proxy. Today it is only a parameter of the free `perform_https_exchange`.
    - `address_policy` and `host_resolver` are public SSRF-off switches. That matches the old `resolver`'s standing, but under ADR-070 a non-default value is a delegated decision and must be audited.
    - Make these test-only (a separate test type or constructor), or emit an audit record when any of them is non-default.

12. **The D5 clamp ("<= 0 means default, no unbounded mode") has no gate.** G8's mutant sets `HttpOptions::total_timeout = 0` downstream of the clamp. Add a unit row on the clamp function, or an injectable default, so "clamp removed" fails.

13. **I4 is claimed with no gate.** `HttpResponse::address` is called "audit data only" but nothing records it. Either emit an egress audit record (grant entry, resolved address, status, body bytes, code) or drop I4 from the header. Also, `egress_error` messages must never include the resolved address. Today's tool path returns the code to the model; it is fine to keep that, but do not add topology.

14. **G3's C5 rows with the real resolver depend on the platform.** Windows `getaddrinfo` may send `2130706433` to DNS or report it unresolvable rather than decoding it. glibc decodes it, so it ends up blocked. Assert "no connect" (`accept_count()==0` on a 127.0.0.1 listener) rather than a specific code, and do not touch live DNS in CI.

15. **The private runtime's `lanes = 1` thread is never used** (all engine work stays on the host). This goes away with finding 4.

16. **The v4 table is pre-existing and not touched by this ADR, but the ADR now calls it "the single table".** It omits 192.0.0.0/24, 198.18.0.0/15 (benchmarking, and fake-IP proxy setups), the 192.0.2/198.51.100/203.0.113 documentation ranges, and the host's own public interface addresses (hairpin). Record these as an inherited residual rather than implying completeness.

### Checked and sound (do not touch)

- **Resolve once, filter every address, connect only to an accepted one.** No re-resolve (`rt/dns.hpp:263-305`). Mixed public and private answers connect only to the public one, which matches sync `resolve_and_validate` (`net_egress_proxy.cpp:362-364`). DNS rebinding between gate and connect is closed.
- **An empty policy fails closed before any lookup** (`dns.hpp:259-262`, `http.hpp:662-667`). A throwing policy rejects.
- **D3, refusing by family:**
  - The PAL keeps v6 as v6 and never unmaps v4-mapped addresses (`resolver_getaddrinfo.cpp:68-74`).
  - Numeric v6 literals skip the resolver and hit the policy.
  - Every v6 form is refused without decoding embedded v4. Correct, and the right call over a hand-written v6 table now.
- **`ipv4_only_resolver`** keeps AAAA-only hosts as `net.host_unresolvable`, as today.
- **C5:** strict `parse_ip_address`, then the policy runs on the binary address (`dns.hpp:228`).
- **C10:** no redirect is followed on either path.
- **TLS:** `HttpRequest::host` (the allowlisted name) is what `ctx.tls` verifies against (`http.hpp:704`). Both paths share `VERIFY_REQUIRED`, the TLS 1.2 floor and `mbedtls_ssl_set_hostname` (`mbedtls_client_config.cpp:80,97`). An IP-literal https allowlist entry behaves identically on both paths; adding a G11 row for it is worthwhile, but nothing changes.
- **D7 closes real holes in today's sync path:**
  - a guest `Content-Length` forwarded unchecked, which is request smuggling (`build_raw_request:404-412`);
  - a duplicate guest `Host:`;
  - absolute-form targets from the guest.
- **D1:** a stop token rather than `EffectContext` is the correct I2 argument, and having no default argument is right.
- **The `total_timeout` timer starts before DNS** (`http.hpp:681-691`), so it covers resolve, connect, TLS and body.
- **No decompression:** `rt::http` does not decode gzip (Transfer-Encoding must be exactly `chunked`), so there is no decompression-bomb path. The chunk-size line is capped at 1 KiB.
- **Byte cap:** `max_body_bytes = min(grant, 16 MiB)` only narrows.
- **WASM side:** collapsing blocked and unresolvable into `host-not-allowlisted` leaks no topology to the guest.
- **Section 1.1:** the corrections to the design doc (2 real call sites, no `wall_ms_cap` on `NetOut`, per-call `HostEgressProxy{}`, the Host/NUL holes) are all verified true.

## ADR-241 red-team, round 2

Result: 0 FATAL, 2 MAJOR, 8 MINOR new. Of the 16 round-1 findings, 11 are closed, 4 partially closed, 0 not closed, 0 made worse, and 1 (#15) is closed by an accepted justification. Folding the sync edge into the async path (D2/8.3) is the right call; it removes most of round 1's problems.

There are two things to fix. First, the WASM edge enters the shared path with an empty control value, so the new single deadline and cancel do not reach it. Second, the global admission cap is a new way for one session to deny egress to every other session. Its stated reason, "no hook reaches fetch", is false.

### Part 1 -- verdicts on round-1 findings

| # | Verdict | Evidence / remaining gap |
|---|---|---|
| 1 interim 1xx / byte total | **Closed (design)** | D0. Two details are in N7: where bytes are counted, and the `0` sentinel. |
| 2 TLS class table | **Closed** | `failure` is carried through `HttpTlsHandshake`. The table rows match `mbedtls_client_config.cpp:66-110`, `http.hpp:201`, `http_tls.hpp:92`. G11 now expects `net.tls_certificate_rejected`/policy. |
| 3 stop + scope + deadline | **Partially closed** | Closed for tool callers (D1, merged `scope_stop_token`, `ctx.deadline`). **Not closed for the WASM edge**: 8.3 calls `fetch(req, granted, {})` (N1). |
| 4 lane-blocking join | **Closed** | Nothing ever destroys or joins the context. New exit-time residuals are in N3. |
| 5 provider DNS-pool coupling | **Closed** | The context has its own DNS pool and there is no borrowed `Runtime*`. G13c is weak as a proof (N8). |
| 6 two implementations / v6 truncation | **Closed** | One code path. `VerifiedEndpoint` is gone from the proxy. |
| 7 sync HTTPS/DNS unbounded | **Closed** | The `rt::http` timers cancel connect, TLS and reads (`http.hpp:681-704`). The WASM deadline value is still missing (N1). |
| 8 G1 cannot kill its mutants | **Closed** | `egress_gate_calls()` checks identity, and G2 mutates each layer on its own. A small test-hygiene issue is in N10. |
| 9 managed headers / compatibility | **Partially closed** | Stripping replaces rejection. But `net.invalid_request` still maps to `host-not-allowlisted` (table section 9), which misleads guest authors about a bad method, header name or target. Map it to `network-error`, or say why not. |
| 10 body copy in gate | **Partially closed** | The gate no longer copies, but the WASM edge now copies the body 4-5 times (N9). |
| 11 public seams | **Partially closed** | Moved behind a testing header, but the friend function plus the lint scope leave a hole (N5). |
| 12 clamp gate | **Closed** | `effective_total_timeout()` unit rows (G8). |
| 13 I4 | **Closed** | The claim is dropped and recorded as R10. `ToolInvocationAudit` covers attribution at tool level. |
| 14 G3 platform dependence | **Closed** | The assertion is "no connect"; real-resolver rows are kept out of CI. |
| 15 unused lane | **Closed by accepted justification** | 8.2 is correct: `Runtime::spawn` (`runtime.hpp:548-579`) is the only non-blocking root launcher, and `block_on` is refused on lanes (`lanes.hpp:209` `ScopedBlockOnRefusal`, `block_on.hpp:329`). The cost of that lane is N3/N4. |
| 16 v4 table | **Closed** | R9. |

### Part 2 -- new findings

#### N1. MAJOR -- The WASM edge passes `EgressCallControl{}`: no stop, no deadline. The guest's `wall_ms` and session cancel cannot reach a parked guest fetch.

**What happens.** 8.3 runs `fetch(req, granted, {})` as a root on the egress context. That root's `rt::scope_stop_token()` belongs to the leaked runtime, which never shuts down, so nothing ever stops it.

**Why the guest's own limits don't help.** The guest's wall budget is wasmtime epoch interruption: `wasmtime_context_set_epoch_deadline(wctx, ticks_for(inst.limits.wall_ms))` at `wasm_backend.cpp:761,853`. Epochs are only checked while guest code runs, never inside a host import. So:
- A plugin with `wall_ms = 2000` that calls `http-request` against a trickling allowlisted peer holds the host lane for the full 120 s.
- The trap fires only when control returns to the guest.
- `invoke_tool`'s "likely_timeout" heuristic (`:1001-1002`) then mislabels the cause.

**Cancel and shutdown.** Session cancel (`ctx.cancellation`) and host `Runtime` shutdown cannot reach it either. The host lane is parked on a latch, so host `~Runtime` waits on that root (`runtime.hpp:441-448`) for up to `total_timeout`.

**Section 7 overclaims.** It says "every caller, including the WASM import, is released by `min(total_timeout, deadline)`". For WASM, that is only `total_timeout`.

**Required changes.**
- The invoke path already has the `EffectContext` (`invoke_tool(handle, req, ctx)`, `wasm_backend.cpp:995`). Thread `ctx.cancellation` and a deadline through `CallCapabilities` (`:383-386`) into `EgressCallControl`. The deadline is the call's start plus `inst.limits.wall_ms` when that is non-zero, and is narrowed by `ctx.deadline`.
- `fetch_blocking` keeps the `RootHandle` and registers a `std::stop_callback` on the caller's token that requests stop on the root. Without that, the token cannot cross onto the egress root.
- **Gate:** add a G7 WASM row. A `wall_ms = 500` plugin calling a trickling peer must return within about 1 s. A ctx stop requested mid-fetch must return `canceled` in under 1 s. Destroying the host Runtime must complete in under 1 s.
- **Mutant:** `fetch_blocking` passing `{}`.

#### N2. MAJOR -- The global `kMaxInFlightEgress = 64` with fail-fast `net.egress_busy` lets one session deny egress to all others. The reason given for not partitioning it is false.

**Scenario.** One tenant fans out 64 allowlisted fetches against peers that trickle 1 byte every 89 s. It can do this through workflow fan-out, `agent.spawn` children, or 64 sessions of one principal. Every other session in the process then gets `net.egress_busy` for up to `total_timeout` (120 s), and can be re-denied indefinitely.

**Is this worse than no cap?** No. With no cap, sockets, DNS queue and buffered bodies (16 MiB each) are unbounded, and today's sync design pinned host lanes, which stalls the whole engine. A cap is necessary. A *global, fail-fast, unpartitioned* cap, however, adds a precise cross-session denial switch that the async design otherwise would not have.

**8.2's reason is false.** It says no per-session cap is possible because "`fetch` sees no session object". The caller builds `EgressCallControl` from `EffectContext`, which carries host-derived identity: `principal` (`core/effect_context.hpp:39`) and `run_id` (`:66`). An admission key is a value that only selects which bucket to debit, so it can only narrow and is I2-neutral, exactly like the stop token. On the WASM edge the same ctx is available (N1).

**Required changes.**
- Add `EgressCallControl::admission_key` (host-derived: the root run id, or the principal; never model-derived).
- Per-key cap, e.g. 8, plus the global cap of 64.
- Children created by `agent.spawn` or workflow fan-out charge the **root** run's key (the ADR-193 lineage already flows to the root), so fan-out cannot multiply the quota.
- Optionally queue for a short bounded time before `egress_busy`.
- **Gate:** session A fills its cap with tricklers; session B's fetch to a fast peer succeeds. **Mutant:** key ignored (one shared bucket).

#### N3. MINOR -- Leaked `Runtime` threads at process exit (refines the team lead's suspicion; partly refuted)

**Refuted.**
- *Lane deadlock:* the root never needs the WASM thread. The egress lane only ever runs sync-edge roots, so no WASM import runs on it, and the reactor thread never runs engine code.
- *Session state:* `fetch` takes request and grant by value. Arguments are copied into the frame when the call is created, on the WASM thread, and `EffectContext` is never passed. Nothing session-owned is touched from the egress lane.
- *A lane resuming a frame whose owner is gone:* posts to a host strand keep the `Scheduler` alive through `StrandCore::sched_` (`lanes.hpp:125-139`). A late egress-reactor completion after a host Runtime is destroyed is queued and never run (a leaked frame), not a use-after-free.

**Real residuals.**
- (a) The egress threads are alive during static destruction. An abandoned `getaddrinfo` (R1) can still be running inside libc/NSS while `exit()` tears statics down. Its finishing path touches only the pool (`offload.hpp:464`) and the global `chain_detail::Registry` (a function-local static) for sync-edge roots.
- (b) A host that embeds AgentEngine in a DLL and `FreeLibrary`s it would leave threads executing in unmapped code. The engine is a static library today, so no current build does this.
- (c) The egress reactor now outlives every host home *by construction*. Before, each Runtime joined its reactor before its lanes died.

**Required changes.**
- State (a) and (b) in R5.
- Add a G13 ASan row (L): a fetch whose per-attempt timer fires at the same time as its frame is destroyed, after its host Runtime is gone. That proves the "queued, never run" claim instead of my reading of it.

#### N4. MINOR -- The latch on a host lane works around the section 4.3 refusal without saying so

`block_on` is refused on lanes (`lanes.hpp:209`) because a parked lane is the hazard. `std::latch` parks the lane exactly the same way. This is no worse than today, because the blocking sync exchange already parked the lane. But the ADR should say so, bound it (after N1: `min(total, wall_ms, ctx.deadline)`), and note that `lane_stall_bound` watchdogs will report it.

#### N5. MINOR -- The `EgressTestSeams` friend function is not a seal

A `friend HostEgressProxy make_test_proxy(EgressTestSeams)` declared in the production class can be *defined* by any translation unit, which gives it private access without the testing header. The G15 lint scans only `include/` and `src/`, but the deployer's own code is where a non-default seam would actually appear.

Host code is trusted, so this is not an I2 break. But the claim "cannot arise in a production binary without that include being visible" is false, and ADR-070 wants a non-default delegated decision to be audited, not hidden.

**Required changes (one of):**
- Put the seams in a separate type, `TestEgressProxy`, that only exists in the testing header and is passed as a `NetEgressBackend`.
- Or keep the friend but emit an audit/log line when a non-default seam is used.

Fix the wording either way.

#### N6. MINOR -- Strip-and-recompute is consistent at the HTTP/1.1 layer; CL.0 is the remaining class

**Refuted: smuggling through re-framing.** Suppose a guest sends a chunk-encoded body as raw bytes with `Transfer-Encoding: chunked`. The proxy strips the TE and sends `Content-Length: <raw length>`. The server reads exactly those bytes as an opaque body, so the framing always matches the bytes sent (`build_request`, `http_wire.hpp:247-258`). The only effect is corruption of the guest's own data; that is worth one sentence in `wit/ae-tool.wit`.

**Remaining class: GET/HEAD with a body.** A body on GET or HEAD is still sent with a recomputed `Content-Length`. Some origin servers ignore CL on GET, so a body of `GET /admin ...` is read as a second request: the CL.0 desync. That reaches a back end behind the allowlisted CDN, and with front-end pooling it can affect other users of that site. This is pre-existing (the sync path did the same), not introduced here.

**Required change:** refuse a non-empty body on GET/HEAD/OPTIONS/DELETE/TRACE in `egress_gate` (contract), or record it as a residual.

#### N7. MINOR -- D0's counting needs pinning down

- **Count where bytes enter.** Count at the transport `read_some` return, once. The mutant "not incremented for leftover bytes" implies counting at decode points, which double-counts or misses the head/body leftover. Specify: counted when received, never when decoded.
- **The `max_response_bytes = 0` sentinel collides with cap 0.** With cap 0, the derived total is `0 + 2*0 = 0`. Write that "derived" is computed when the limits are built, and that a computed 0 means 0 bytes allowed, not "unlimited".
- **The counter sees TLS plaintext, not TCP bytes.** TLS-layer traffic (TLS 1.3 KeyUpdate or NewSessionTicket floods, empty records) is uncounted and bounded only by `total_timeout`. State that.
- **Count G9 on the client side.** As written, G9 counts "at the peer". Bytes the peer sent include kernel buffer slack (autotuned windows of several MiB), so the test is flaky or must allow slack. Count on the client, through the transport or a test hook.
- **Team-lead suspicion refuted:** D0 does not regress provider streams. Streamed bodies are already capped at `effective_body_cap()` on wire bytes (`http_wire.hpp:516,606,654`), so an SSE stream over 16 MiB is already cut off **today**; that is pre-existing, not this ADR's doing. `max_response_bytes` only adds head allowance on top. No keep-alive exists (`Connection: close`), so nothing accumulates across requests.

#### N8. MINOR -- G13c cannot fail

"Saturate the egress DNS pool; a lookup on *another* pool still completes" is true by construction for any two `OffloadPool`s. Its named mutant (a borrowed Runtime's pool) cannot be expressed now that the `Runtime*` parameter is gone.

**Required:** drive a real provider client (its own `rt::Runtime`) while the real egress path is saturated. Otherwise drop the gate and keep the structural argument.

#### N9. MINOR -- The WASM edge copies the guest body 4-5 times

The copies are: the WIT list into `req` (`wasm_backend.cpp:540-548`), the request into the root frame, into `HttpRequest`, into the wire string in `build_request`, and into the `std::vector<std::byte>` at `http.hpp` step 5. They are bounded by `memory_bytes`, but R2-10's "no body copy" is true only of the gate. Move through the chain.

#### N10. MINOR -- `egress_gate_calls()` is a process-global relaxed counter

Any other fetch in the same test binary (live or parallel rows) changes it. Snapshot and diff it under a test-only mutex, or count per proxy through the testing header.

### Checked and sound

- **The single code path.** `fetch_blocking` = spawn + latch; nothing runs on the waiting thread; no deadlock. The guest now gets the same parser, cap, Host, codes and TLS class as tool callers.
- **The context:** its own reactor and DNS pool; created lazily under a mutex; failure returns `resource` and the next call retries; no `Runtime*` parameter; nothing joins on an engine thread.
- **Admission accounting:** the RAII slot is released on frame drop.
- **Merged stop** for tool callers, following the `sse_stream_pump.hpp:203-205` precedent.
- **`effective_total_timeout`:** clamps `<= 0`; an expired deadline fails before resolve.
- **TLS class carry-through** and the rebuilt error table.
- **D0 design:** a shared head budget, an interim bound of 8, and a request-level total. It fixes the provider plane too, without regressing it (N7).
- **Strip-then-validate order.** A header like `Content-Length ` (trailing space) is not stripped but is then rejected as a non-token.
- **No change to D3 or the SSRF core** since round 1: still refuse by family, the IPv4-only resolver, resolve once, connect to the verified address, TLS checked against the allowlisted name, and no redirects.

## ADR-241 red-team, round 3 (final)

**Result:** 0 FATAL, 2 MAJOR, 6 MINOR.
- **Round-2 findings:** of N1-N10, 7 are closed, 2 closed with a residual, and 1 partially closed.
- **The two MAJORs:**
  - the admission key collapses to a single bucket of 8 in the most common deployments;
  - the slice plan's S2 is not independently shippable as written.
- **Neither MAJOR touches S1.** S1 is ready to prove now.

### Part 1 -- verdicts on round-2 findings

**N1 (the WASM edge passes `{}`): Closed.** The fix relies on:
- `invoke_tool` holding `EffectContext& ctx` (`wasm_backend.cpp:824-825`);
- `CallCapabilities` (`:383-386`) being per-call and reachable from `cb_http_request`;
- `RootHandle::request_stop()` existing (`rt/runtime.hpp`, RootHandle, `state_->stop.request_stop()`);
- `fetch` merging the egress root's `scope_stop_token()`;
- G7's WASM row with the `{}` mutant.

Two requirements on the implementation:
- **Ordering:** `spawn` first, then register the `stop_callback`. A stop already requested fires inline on registration, which is correct, but only after `h` exists.
- **The deadline is measured from the start of the *call*, not the import.** That is right: guest work before the import uses up `wall_ms`.

**N2 (global cap, no partition): Closed in kind, replaced by new MAJOR M1.** The withdrawn claim is gone and a per-key cap exists. The key chosen creates M1.

**N3 (leaked threads at exit): Closed as a residual.** R5 (a)-(c) states the exit, DLL-unload and outlives-every-host facts. G13g (ASan, L) proves the "queued, never run" behaviour instead of asserting it.

**N4 (latch parks a host lane): Closed.** It is stated, bounded by `min(total, wall_ms, ctx.deadline)`, and the watchdog reports it.

**N5 (friend not a seal): Closed with a wording residual (m4).** There is no friend or setter on the production type; `TestEgressProxy` is separate. However, the "shared free functions taking the seams as parameters" are themselves a seam on the production side; see m4.

**N6 (CL.0 through GET/HEAD bodies): Closed.** G16 refuses a body on GET/HEAD/TRACE/OPTIONS. DELETE is honestly residual (R13).

**N7 (D0 counting details): Closed.**
- Counting is once, at the `read_some` return.
- `std::optional` removes the 0 sentinel; a computed 0 means 0 bytes allowed.
- TLS-layer traffic is stated as uncounted.
- G9 counts on the client.
- The one remaining point is m3.

**N8 (G13c could not fail): Closed by dropping the gate.** The structural argument is acceptable: there is one context-owned pool and no `Runtime*` parameter, so the shared-pool mutant cannot even be written.

**N9 (body copies): Partially closed.** The body is moved down the chain, giving 3 copies at peak, and 2 after S4. The memory bound the ADR claims is false; see m1.

**N10 (global gate counter): Closed.** A per-instance `GateObserver*` replaces it; production passes null.

### Part 2 -- the three deliberate deviations

#### (a) Admission key = `tenant_id + "/" + principal.root_id()`

**Can a guest, tool or model influence it? No (I3 holds).**
- `Principal` comes from:
  - the host: `agent_session_core.cpp:951`, the session's `principal_`;
  - per-request authority from the ADR-061 bridge (`:945`): a bearer credential terminated by host code.
- A child's id is `derive_spawn_child_id(caller_ref, caller_principal, seq)` (`agent_spawn.hpp:381-382`), all host values.
- `derive_on_behalf_of` sets `delegation_root = parent.root_id()` unconditionally and copies the tenant (`trust/principal.hpp:111-125`). It is the only derivation site (`agent_spawn_child_run.hpp:155-156`).
- Nothing model-derived feeds the key.

**Can a child mint a fresh key? Not through spawning. It can through restore (m2).**

**Does keying per principal starve that principal's own sessions? Yes. In the common deployments that is not acceptable as specified; see M1.**

#### (b) Three body copies at peak

There is a memory consequence, and the ADR's bound is wrong. See m1.

#### (c) `max_response_bytes` as `std::optional<std::uint64_t>`

Fine, with one overflow edge. See m3.

### Part 3 -- new findings

#### M1. MAJOR -- Per-principal keying turns the cap into a global cap of 8 for single-principal and anonymous deployments

**Scenario.**
- The embedded host model is one service principal for everything (`make_embedded_principal`, `trust/principal.hpp:70`).
- A session whose host never sets a principal has `Principal{}`: an empty id and empty tenant (`agent_session_core.cpp:714`). Every such session gets the key `"/"`.
- In both cases, every session in the process shares **one bucket of 8**.
- One session (or one workflow fan-out) with 8 fetches to a trickling or slow allowlisted peer holds them for up to 120 s. Every other session's egress then fails `net.egress_busy`.
- That is strictly worse than round 2's global 64, and worse than today, where each fetch pinned only its own lane.
- Legitimate parallelism over 8 in one tenant also starts failing (`resource`, not retried).

**Why it happened.** The deviation was forced because "no root run id reaches `EffectContext`". The fix is to put one there, not to key by principal.

**Required changes.**
1. Key by **root run**:
   - add `EffectContext::root_run_id`;
   - the session core sets it to its own `run_id` for a root;
   - `ChildSpawnRequest` carries the parent's `root_run_id` into the child (next to the principal, `agent_spawn.hpp:534`);
   - background jobs copy it from the job spec.
2. Keep the tenant/principal prefix as a **second, larger** bucket (e.g. 32) so many runs of one principal cannot take all 64.
3. Make both caps host-configurable on the context.
4. **Gate G13h:**
   - two root runs of the **same** principal: run A saturates its cap and run B still fetches;
   - a child of A still charges A.
   - Mutant: the key is the principal only.

**If a new `EffectContext` field is rejected**, the minimum fix is to make the per-key cap host-configurable with a default of 64 (equal to the global cap), so per-key is opt-in for multi-principal hosts. Document that single-principal hosts get no per-run fairness.

#### M2. MAJOR -- S2 is not independently shippable; its gate list depends on S3

**S2 ships the gate (with `validate_request`) on the WASM edge, but stripping only arrives in S4.**
- `fetch_blocking` "already calls the gate" in S2, and the gate's `validate_request` is `build_request`'s rule set: it **rejects** a caller-supplied Host, Content-Length, Transfer-Encoding or Connection (`http_wire.hpp:238-241`).
- So from S2 until S4, every WASM guest whose HTTP library sets `Host` or `Content-Length` breaks. Until S3 that failure is mapped to the misleading `host-not-allowlisted`.
- This is round-1 #9's compatibility break, shipped for two slices.

**S2 deletes the `resolver` field while the WASM edge still runs the old exchange.**
- The old `fetch_blocking` can only use `resolve_and_validate` directly.
- `TestEgressProxy`'s `host_resolver`/`address_policy` cannot reach it.
- The ~12 existing sync-path tests that reach a loopback server through `resolver` lose their seam. The sync edge ships S2 with *less* coverage than today.

**S2's gate list names rows that only S3 can satisfy:**
- G1 "`fetch_blocking` calls the gate once before the first `host_resolver` call": the old exchange never calls `host_resolver`;
- G10 "on both entry points";
- G11 "through BOTH";
- G12 "on both entry points";
- G13e/h, which count sync-edge calls against the caps (the old exchange is not admitted).

**Is one release of divergence a hole?** Not a security hole. The old WASM path keeps `resolve_and_validate`, connect to the verified address, no redirects and C9. It is today's behaviour plus a stricter gate. But it is an unproven state with a compatibility break, under a plan that claims each slice is independently shippable.

**Required changes (one of):**
1. Fold stripping and the GET-body refusal into S2 (S4 shrinks to the write-vector move and the WIT note), **and** merge S3 into S2 so both entry points move together. This is the preferred option.
2. Keep S3 separate, but in S2 have `fetch_blocking` keep calling the old exchange **without** the gate and keep the `resolver` field until S3. Scope S2's gates to the `fetch` half only. Move G1-sync, G10/G11/G12-sync and G13e/h-sync to S3.

**S1 is clean either way.** It is self-contained in `rt/`, its gates (G0 R1-R5, the D0 mutants, the cap-0 row at the rt level) depend on no later slice, and it re-runs the provider suite.

#### m1. MINOR -- Three host copies of a request body that nothing caps; the "bounded by `memory_bytes`" claim is false

**The claim fails in two ways.**
- The limiter is installed only when `inst.limits.memory_bytes > 0` (`wasm_backend.cpp:848-850`). Otherwise the bound is wasm32's 4 GiB.
- The copies are on the **host** heap, outside that limit anyway.

**The worst case.** No grant field caps the request body; ADR-011 C8 caps responses only.
- One `http-request` with a 1 GiB body costs about 3 GiB of host memory at peak.
- With 8-64 concurrent calls, that is a host OOM driven by a guest.
- This was pre-existing at 2 copies; it is now 3.

**Required:**
- `egress_gate` refuses a request body over `min(byte_cap.value_or(16 MiB), 16 MiB)` (or a separate host-set `max_request_bytes`) as `resource`, before any copy beyond the WIT one.
- Gate: a 17 MiB body gives `resource` and 0 resolver calls.

#### m2. MINOR -- Restored sessions escape the root bucket

`restore_from_record` rebuilds `Principal{rec.principal_id, rec.principal_tenant_id}` (`agent_session_core.cpp:800`). That drops `delegation_root`, which is acknowledged in the `principal.hpp:51-54` comment. A restored delegated child's `root_id()` is then its own id, so it gets a fresh bucket.

Spawned children run in place today, so this is narrow. Note it as a residual, or close it with M1's `root_run_id` persisted in the record.

Related: the key joins `tenant_id + "/" + root_id` with an unescaped `/`. Tenant `a/b` with root `c` and tenant `a` with root `b/c` collide, so two tenants share one bucket. Inbound principal ids can come from credential subjects. Use a structured key (a pair) or a length prefix. Also, the per-key table needs an erase-when-zero rule. The ADR's "two atomic counters" cannot implement per-key, and a never-shrinking map grows with every inbound principal.

#### m3. MINOR -- `max_response_bytes` derivation can overflow

`max_body_bytes` is a public `uint64` that any caller may set (for example to `UINT64_MAX` to mean "the ceiling"). Then `max_body_bytes + 2*max_header_bytes` wraps to a tiny total, and every response fails.

Derive from `effective_body_cap()` (already clamped to 16 MiB, `http_wire.hpp:106-108`) with saturating arithmetic. Add a unit row.

Also state whether S1's TLS `failure` carry-through changes the **provider** plane's class mapping (`provider_chat_wire.hpp:72`). If it does, that is a provider behaviour change S1 must flag. If it does not, say that the provider ignores `failure` for now.

#### m4. MINOR -- The shared free functions are the seam now

`TestEgressProxy` "shares the production gate/`fetch` implementation as free functions taking the seams as parameters". Those functions must be declared somewhere `TestEgressProxy` can reach. If that is a production header (`net_egress_proxy.hpp` or an exported `detail`), any host code can call them with a permit-all policy, without including the testing header. G15's lint watches only the testing header.

Put the declarations in a `sandbox/testing/` or `sandbox/detail/` header covered by the same lint, or reword the claim to "the production *type* has no seam".

#### m5. MINOR -- The fail-fast `egress_busy` has no retry guidance

When it is `resource`-class and returned to the model, the model will often just call the tool again in a tight loop. Include a hint in the message (busy, retry later). A short bounded wait before failing (e.g. up to 1 s, capped by the deadline) would make transient saturation invisible.

#### m6. MINOR -- The R12 lane-serialisation residual should give a number

With 64 sync-edge roots parsing up to 16 MiB each on one egress lane, every sync-edge continuation (including cancels being delivered) queues behind up to about 1 GiB of parsing. Either give R12 a measured figure in the prove step, or use 2 lanes. The lanes are idle otherwise.

### Checked and sound

- **The key is never model-derived (I3).** Its sources are the session principal, ADR-061 authority, and `derive_on_behalf_of`, which unconditionally sets the root and copies the tenant; children cannot mint a root.
- **N1's WASM control plumbing:**
  - wall time is measured from the start of the call;
  - an already-expired deadline fails before resolve;
  - the stop is forwarded through `RootHandle::request_stop`, and its callbacks only post.
- **G7 WASM row and its mutant**, G13g ASan, G16, the `optional` sentinel, and counting at `read_some`.
- **S1's independence and its gates.**
- **Everything marked sound in rounds 1 and 2:** the SSRF core, refusing by family, the TLS class carry-through, the single code path, the leaked context, strip-then-validate, and the merged stop.

### Overall verdict

**YES: S1 (D0 in `rt::http`) can go to the prove step now.** It is self-contained and unaffected by any open finding; it only needs the overflow-safe derivation from m3 and an explicit statement on the provider TLS class.

**S2 is blocked** until:
- M1 is resolved: a root-run key (or a per-key cap defaulting to the global one);
- M2 is resolved: fold stripping into S2 and merge S3 into it, or re-scope S2's gates to the `fetch` half;
- m1's request-body cap is added to the gate.

## ADR-241 red-team, round 4 (S2 only)

**Result:** 0 FATAL, 2 MAJOR, 4 MINOR.
- **Round-3 findings:**
  - M2 is closed by the merged S2.
  - M1 is partially closed: run-level keying is right, but an empty key skips its level instead of failing closed, and the start-site table misses the MCP server.
  - m1 and m3-m6 are closed; m2 is closed with a stated residual.
- **New:** the principal level keys on `principal.id`, so every `agent.spawn` child gets a fresh 32-slot principal bucket. Fixing that, and the empty-key rule, unblocks S2.

### Part 1 -- verdicts on round 3

**M1 (keying collapsed to one bucket of 8): Partially closed.**
- Right:
  - the structured `AdmissionKey` (no joined string);
  - per-root-run buckets;
  - two runs of one principal isolated (G13h);
  - children charging the root run (8.4 rows 2-4).
- Wrong:
  - an empty key component **skips** its level rather than failing closed (N-M1);
  - the start-site table omits `protocol/mcp/server.hpp:370,430` (N-M1);
  - the principal level uses `principal.id`, not `root_id()` (N-M2).

**M2 (S2 not independently shippable): Closed.**
- S2 now moves both entry points, the strip, the GET-body refusal and the request cap together.
- Every gate it names is satisfiable inside S2 (Part 3).

**m1 (uncapped request body): Closed.**
- `max_request_bytes` is checked in the gate.
- The import checks `byte_list.of.list.size` **before** `reserve` (`wasm_backend.cpp:540-548`), so an oversized body is never copied.
- The peak is now stated as `<= 3 * max_request_bytes * 64` (2 after S3). G17's mutant is "the import checks after the copy". Good.

**m2 (restore escape, key collision, never-shrinking map): Closed with residual.**
- G13j covers the collision and erase-when-zero.
- `root_run_id` is persisted in the session record (8.4, last row).
- `delegation_root` is still dropped on restore (`agent_session_core.cpp:800`). That is honestly stated; it matters again once N-M2 switches the principal level to `root_id()`.

**m3 (derivation overflow; provider TLS class): Closed.**
- The derived total uses saturating arithmetic from `effective_body_cap()` (`http_wire.hpp:106-108`), and R6 has unit rows.
- S1 has no TLS change. The remaining point is minor: S2 adds `failure` to `rt::HttpResponse`, which the provider plane shares, so say in S2 that `provider_chat_wire.hpp:72` ignores it (no class change for model calls).

**m4 (shared free functions are a seam): Closed.** They live in `sandbox/detail/net_egress_impl.hpp`, under the same lint.

**m5 (no retry guidance on `egress_busy`): Closed.** There is a bounded 1 s wait (capped by deadline and stop) and a retry-hint message. G13e checks both "waits" and "succeeds if a slot frees".

**m6 (one lane serialises sync-edge parsing): Closed.** Two lanes, with R12 measured in the prove step. Editorial: R12's last line is duplicated ("serialises their parsing on it ...").

### Part 2 -- attacking `root_run_id` and admission

#### N-M1. MAJOR -- An empty key fails OPEN into the global bucket; the table misses a real start site whose keys are empty

**The rule (8.4):**
- empty `root_run_id` falls back to `run_id`;
- if both are empty, **the run level is skipped**;
- an empty principal **skips the principal level** (8.2).
- So a call with an empty identity is held only by the global 64.

**The missing start site: the MCP server.**
- Both `tools/call` handlers build a fresh `EffectContext ctx; ctx.principal = caller;` (`protocol/mcp/server.hpp:370` sync, `:430` task-augmented). They go straight to `invoke_tool`/`background_task`, never through `agent_session_core.cpp:211`.
- The table's claim that "the MCP/A2A request path (ADR-061 Tier 3)" goes through `:211` is false for MCP `tools/call`.
- There, `run_id` and `root_run_id` are always empty.
- `caller` is empty for an unauthenticated request. The server already treats `caller.id.empty()` as unauthenticated (`server.hpp:260-264`, ADR-061 §7 R13).

**Can that reach egress?**
- `read_content` cannot: on the no-grant path `ctx.capabilities` is null, and the tool refuses.
- A **WASM tool** can. Its NetOut comes from the plugin's *operator* grant (`inst.operator_grant.bind`, `wasm_backend.cpp:835-838`), not from `ctx.capabilities`.
- So an unauthenticated remote MCP client calling an exposed WASM tool with a NetOut grant is held only by the global 64. It can deny egress to every session in the process: exactly R1's blast radius, from an anonymous network caller.

**Other paths that fail open the same way:**
- direct host dispatch with a hand-built ctx (8.4 row 7);
- a workflow whose supervisor cannot stamp an id ("if not, ... R14", 8.4 row 4);
- any future start site nobody adds to the table.
- G13i *enshrines* the skip ("an empty principal is not subject to the principal level").

**Required changes.**
- **Empty never means "skip".** An empty run identity charges a dedicated **unattributed** run bucket. An empty principal charges a dedicated **unattributed** principal bucket. Both are small and host-configurable (default 8). Unattributed callers then share one small bucket instead of the whole pool. This also covers future start sites, so the table no longer has to be complete to be safe.
- Add rows for `mcp/server.hpp:370,430`. The server sets `run_id`/`root_run_id` to a per-request id it mints, e.g. `mcp:<request id>`; or leave them empty and rely on the unattributed bucket.
- **Rewrite G13i:**
  - an empty principal charges the unattributed principal bucket;
  - 9 concurrent empty-identity calls means the 9th waits and then gets `egress_busy`, while an attributed principal still succeeds.
  - **Mutant:** "empty skips the level".
- Add an MCP row to G13: an unauthenticated `tools/call` of a NetOut WASM tool is bounded by the unattributed bucket.

#### N-M2. MAJOR -- The principal level keys on `principal.id`, so every spawned child gets a fresh 32-slot bucket

**What happens.**
- `admission_of(ctx) = {tenant_id, principal.id, ...}`.
- `agent.spawn` children run as a *derived* principal with a fresh id (`derive_on_behalf_of`, `trust/principal.hpp:111-125`; the child id comes from `derive_spawn_child_id`, `agent_spawn.hpp:381-382`).
- So a child's fetches charge the principal bucket `(tenant, child_id)`, not the root principal's.

**Scenario.**
- Principal P starts 5 root runs (5 sessions, 40 slots of run budget). Only 32 should be allowed at the principal level.
- Each run does its fetches through one spawned child.
- Each child is its own principal at 8 or fewer. P's 32 is never charged.
- P holds 40 slots, and with more runs the full 64, starving every other principal.
- The principal cap is evaded by the very fan-out that 8.2 says "cannot multiply the quota". That claim is made only for the run level.

**Required changes.**
- The principal level keys on `(tenant_id, principal.root_id())` (`trust/principal.hpp:47-50`), the round-3 choice, which is correct **at this level**.
- Keep `root_run_id` for the run level.
- The restore residual (`delegation_root` dropped, `agent_session_core.cpp:800`) then becomes a real, if narrow, escape for restored delegated children. Either persist `delegation_root` with `root_run_id` in the same additive record change, or list it as R14.
- **G13h/i gain a row:** 5 root runs of P, each fetching through a spawned child, are held to 32 in total.
- **Mutant:** the principal key uses `principal.id`.

#### Other questions from the team lead

**Model-derived input (I3): none found.** Every source of `root_run_id` is host-minted:
- `session_id_ + ":run:" + counter` (`agent_session_core.cpp:211`);
- the parent ctx copied into `ChildSpawnRequest` (`agent_spawn.hpp:534`);
- the job spec owner (`background_job_runner.hpp:634`);
- the workflow `run_id_` restored from its own record (`workflow_run_state_record.hpp:416`, `workflow_supervisor.cpp:459`).

Child session ids come from `derive_spawn_child_id(caller_ref, caller_principal, seq)`, not model text. `tool_pipeline.cpp:386` `background_task` takes `ctx` by value and only resets callbacks (copy then sanitize), so the field survives into backgrounded calls. `sanitize_for_detached_worker` (`workflow_as_chat_client.hpp:171-185`) also only nulls pointers and callbacks.

**Different root than the lineage.**
- `agent.spawn`, background jobs, workflow nodes and nested spawns: correct, provided the implementation does three things:
  - `fork_from` (`agent_session_core.cpp:673`) copies `inherited_root_run_id_` with `principal_`;
  - `clear_in_process_state` (`:714`, which resets `principal_`) does **not** reset `inherited_root_run_id_` for a child session. If it does, the child becomes its own root.
  - Add both as G13h rows.
- Each `resolve_interaction` / approval resume that starts a new `run_counter_` run of a *top-level* session is a new root, by design. Any background work from the earlier run keeps charging the earlier bucket. No egress tool is `Backgroundable` today (`read_content`), and WASM descriptors are `captures_session_state` and refused for backgrounding (`tool_pipeline.cpp:415-419`, `wasm_tool_bridge.hpp:131`). So this is not reachable now. Record it as a residual for the first backgroundable egress tool.
- Delegation that crosses a protocol boundary starts a new top-level root on the receiving side, for example an in-process A2A server serving a delegating local agent. The principal level (after N-M2, keyed by the delegation root through the ADR-061 bridge principal) is what bounds it. Note it.

**Races between erase-when-zero and admit: none, if implemented as written.** Decrement-and-erase and admit happen under the same lock, and a live slot holds a count of at least 1, so its entry cannot be erased under it. Two requirements:
- **Admit all three levels atomically.** With one mutex per level, a run-level increment followed by a principal-level failure needs a rollback that also erases-when-zero. In between, other callers see a transient over-count, which shows up as spurious `egress_busy`. Use **one** mutex for both maps and the global count, admitting all or nothing.
- **The RAII slot must hold the key, not a map iterator, and look it up on release.** If limits are lowered at run time, release must still decrement normally.

**Can an empty principal bypass the global cap? No.** The global 64 is always applied. The problem is the opposite: an empty identity gets *only* the global cap (N-M1).

### Part 3 -- the merged S2

**Coherent and shippable as one slice: yes.**
- Both entry points, the gate, the strip, the GET-body refusal and the request cap move together. No intermediate state rejects a header the old WASM path accepted, or loses the sync edge's seam.
- No S2 gate depends on S3. S3 only re-measures G17 at 2 copies and re-runs G2b.
- G9 depends on S1's D0, which ships first. That ordering is correct.

#### n1. MINOR -- S2 is very large (a-h); give it an internal commit order

S2 changes:
- `EffectContext`;
- the session record schema;
- the session core;
- `agent.spawn`, the background-job runner and the workflow supervisor;
- the rt TLS result;
- the gate, both entry points and the WASM backend.

That is shippable, but a single commit would be hard to bisect. Suggested order inside the PR:
1. (a) `root_run_id` plumbing plus its G13h rows (no behaviour change yet);
2. (e) TLS `failure` plus G11;
3. (d)/(f) gate, strip, refusal, cap and `TestEgressProxy`, with G1-G6, G16 and G17;
4. (b)/(c)/(g)/(h) the context, admission and both entry points, with G7-G13.

Each step should be green before the next.

#### n2. MINOR -- The workflow-supervisor row is not decided yet

8.4 row 4 says "Implementation must confirm the supervisor has the id at stamping time; if not, ... R14". After N-M1's unattributed bucket this fails closed, so it is safe. But make the row a prove-step check with a gate: a top-level workflow's N agent nodes share one run bucket. Do not leave it as a conditional sentence.

#### n3. MINOR -- Bounded-wait polling has no FIFO

Retrying every 25 ms lets late arrivals take slots ahead of earlier waiters. That is acceptable for a 1 s bound, but state it.

One more requirement: the wait has to sit after the gate and before resolve. Otherwise gate-rejected calls would wait 1 s and count against the bucket. G13b checks that gate-rejected calls never touch the context, which implies the gate runs first; make the order explicit in 8.2.

#### n4. MINOR -- Say the provider plane ignores `HttpResponse::failure` (m3 remainder)

S2's TLS carry-through adds a field to an rt type the provider plane shares. State in S2 that `provider_chat_wire.hpp:72` is unchanged, or flag it if the provider is meant to start using it.

### Checked and sound

- **I3:** no model-derived input reaches `root_run_id` or any admission key.
- **Lineage propagation** through `agent.spawn`, background jobs and workflow nodes: correct, subject to the `fork_from`/clear requirements above.
- **Structured keys and erase-when-zero:** no race under a single lock.
- **Merged S2:** no dependency on S3.
- **m1's check before the copy**, m3's saturating derivation, m4's lint scope, m5's bounded wait, and m6's two lanes.
- **Everything marked sound in rounds 1-3:** the SSRF core, refusing by family, TLS class carry-through, the single code path, the leaked context, and strip-then-validate.

### Verdict

**NO, S2 is not ready for the prove step yet. Two design edits unblock it:**
1. **N-M1:** empty key components charge small "unattributed" buckets instead of skipping their level; add the MCP server rows (`server.hpp:370,430`); rewrite G13i.
2. **N-M2:** the principal level keys on `principal.root_id()`, with a fan-out-through-children row.

Both are small, local changes. After them, S2 is ready, with n1-n4 handled during the prove step.
