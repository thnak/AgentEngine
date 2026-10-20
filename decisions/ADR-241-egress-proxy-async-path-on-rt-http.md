# ADR-241: the ADR-011 egress proxy's async path on `rt::http`

Status: **Proposed** (2026-10-20) -- designed and red-teamed in four rounds (r1: 0 FATAL/8 MAJOR/8 MINOR; r2: 0/2/8; r3: 0/2/6;
S1 passed the final round, S2 was blocked until this revision); not proven, not Judged.
Issue: #180 (slice E1 of `docs/planning/2026-10-20-issue180-sandbox-conversion-design.md` section 7).
Builds on: ADR-011 (egress proxy, gates C1-C11), ADR-013 (HTTPS), ADR-016 (provider address policy), ADR-193 (delegation
lineage), ADR-237 (4.4-4.6, steps 43-57: `rt::http`, `rt::dns`, `rt::Runtime`), ADR-239 (seams narrowed to synchronous).
Supersedes in part: ADR-011 section 3's "IPv4-only resolve through `resolve_and_validate`" for `HostEgressProxy` only.
Records: the red-team reports of this ADR, rounds 1-3: `docs/planning/2026-10-20-adr241-egress-redteam.md`.
Invariants: I2 (no ambient authority), I3 (model output is data), I8 (budgets enforced). I4 is NOT extended here beyond what
exists today (section 10).
Review tags in the body: **[R2-n]**/**[R3-n]**/**[R4-n]** = the red-team finding that caused the change ("L" = a team-lead
decision); strip them when the ADR is Judged.

## 1. Problem

`HostEgressProxy::fetch` (`include/agentengine/sandbox/net_egress_proxy.hpp:221`) is `co_return fetch_blocking(...)`; it never
suspends. `fetch_blocking` (`src/sandbox/net_egress_proxy.cpp:747-806`) does a blocking `getaddrinfo` (`:337`), `tcp_connect`
+ `select`, a per-wait 90 s idle bound and no overall deadline. A slow or hostile peer pins the lane, `EffectContext::
cancellation` cannot reach the socket, and a trickling peer is unbounded. `rt::http` already is the ADR-011-faithful async
client. E1 moves **both** entry points onto it. **[R2-6/L]** There is no longer a second implementation: `fetch_blocking`
(the WASM `http-request` import, `src/backends/wasm/wasm_backend.cpp:565-567`) drives the async `fetch` to completion (8.3).

### 1.1 Corrections to the design doc (verified)

1. Production `fetch(` call sites: **2** (`tools/read_content.hpp:299`, `tools/extract_pdf_text.hpp:217`); `extract_pdf_{images,
   metadata,toc}` go through `extract_pdf_text_detail::fetch_via_url`. Fakes to change: `tests/sandbox/test_execution_seams_on_lane.cpp:107`,
   `tests/sandbox/test_net_egress_proxy.cpp:202`, `tests/tools/test_read_content.cpp:49`. Direct `proxy.fetch(` test calls: ~14 in
   `test_net_egress_proxy.cpp`, plus `test_provider_egress_address_policy.cpp:326`, `test_llamacpp_live_e2e.cpp:459`, `test_ollama_live_e2e.cpp:409`.
2. `cap::NetOut` has no `wall_ms_cap` (`trust/capability.hpp:125-129`); the deadline cannot come from the grant.
3. The `resolver` field returns a `VerifiedEndpoint` (bypasses any address policy); ~12 tests use it. It cannot survive.
4. Every tool builds `HostEgressProxy{}` per call; `wasm_backend.cpp:566` does too. Nothing may create per-object I/O state.
5. Today a guest `Host:` is forwarded **in addition to** the proxy's own (`build_raw_request :235-262`), and `reject_crlf` (`:295`) ignores NUL.
6. AAAA-only today is `net.host_unresolvable` (transient), not `net.address_blocked` (`resolve_candidates :337`, `AF_INET`).
7. **[R2-6]** `ProviderHttpClient` (`src/sandbox/provider_http_client.cpp:19-35`) still calls `perform_http[s]_exchange[_streaming]`;
   those functions therefore stay exported and untouched. The proxy simply stops calling them. Deleting them is that client's slice.

## 2. Decision summary

| # | Decision |
|---|---|
| D0 **[R2-1/L]** | Prerequisite slice in `rt::http` itself: request-level wire-byte total, shared head budget, bound on interim 1xx. Applies to the provider plane too. |
| D1 **[R2-3]** | `fetch(req, granted, EgressCallControl{stop, deadline})`, no defaults. Stop is merged with `rt::scope_stop_token()`; the per-call deadline narrows `total_timeout`. |
| D2 **[R2-9,10]** | One pure `egress_gate` (C1, C3, C7, C9 + validation) called first by the single code path; managed headers are **stripped and recomputed**, not rejected. |
| D3 | `AddressPolicy`: IPv4-only, refuse every non-v4 family; `ipv4_only_resolver` keeps AAAA-only as `host_unresolvable`. |
| D4 | Byte cap = body wire bytes; heads share `min(64 KiB, cap)`; request-level total `cap + 2*min(64 KiB, cap)`. Same on the WASM edge now. |
| D5 | Non-zero `total_timeout` (default 120 s) for every caller, including the WASM edge. |
| D6 | TLS by `rt::tls_connector`, hostname = allowlisted name; TLS failure class carried through `rt::http`. |
| D8 **[R2-4,5/L]** | One process-wide egress I/O context (a leaked 2-lane `rt::Runtime` with its OWN DNS pool), never destroyed, never joined. No borrowed `Runtime*`. |
| D9 | Error table rebuilt from the real code (9). |
| D10 **[R4-1]** | Admission is two-level by host-set run identity: per root run (8) and per principal (32), both under a global 64; the key is structured; `EffectContext::root_run_id` is new, host-set only (8.4). |
| D11 **[R4-m1]** | The gate refuses a request body over a host-set `max_request_bytes` (default 16 MiB). |

## 3. D0 -- prerequisite: bound what `rt::http` reads [R2-1]

Finding: `open_stream` step 6 (`rt/http.hpp:732-760`) resets `HeadParser` on every non-101 1xx with a fresh 64 KiB budget and no
counter; interim bytes count against neither `max_body_bytes` nor any cap; trailers have a second allowance
(`http_wire.hpp:622-624`, "counted against the head cap"). The claim "worst case = cap + 64 KiB" was false. The provider plane
has the identical hole, so the fix is in `rt::http`, as its own first slice.

Changes (`rt/http_wire.hpp` `HttpLimits`, `rt/http.hpp`):
1. **Shared head budget.** Interim and final heads draw from ONE `max_header_bytes` budget: the parser for the next head is
   built with the remaining budget (not a fresh `HttpLimits`). Exhaustion is `net.header_too_large` (resource).
2. **`HttpLimits::max_interim_responses = 8`.** The 9th interim response fails `malformed_response` (`net.protocol_error`).
   Default for the provider plane: 8 (a legitimate server sends at most one `100 Continue`; 102/103 are rare).
3. **`HttpLimits::max_response_bytes`**, a request-level counter of every byte received from the transport for this exchange
   (head + interim heads + body wire bytes + trailers). **[R3-7]** It is incremented **once, at the `HttpTransport::read_some` return**
   (when received, never when decoded), so leftover bytes handed from the head parser to the body decoder are neither recounted nor
   missed. "Derived" is computed when the limits are built, **[R4-m3] from `effective_body_cap()` (already clamped to 16 MiB, `http_wire.hpp:106-108`) with saturating arithmetic** -- `sat_add(effective_body_cap(), sat_mul(2, max_header_bytes))` -- never from the raw public `max_body_bytes`, so a caller setting `max_body_bytes = UINT64_MAX` (meaning "the ceiling") cannot wrap the total to a tiny value (provider plane 16 MiB + 128 KiB,
   no behaviour change for a legitimate stream) and **a computed 0 means 0 bytes allowed, not unlimited**; the field is
   `std::optional<std::uint64_t>` (unset = derive) so the sentinel cannot collide with cap 0. Overflow is `body_too_large`
   (`net.byte_cap_exceeded`, resource). It counts TLS *plaintext* (what `read_some` returns), not TCP bytes: TLS-layer traffic
   (KeyUpdate/NewSessionTicket floods, empty records) is uncounted and bounded only by `total_timeout`. Not a provider-stream
   regression: streamed bodies were already cut at `effective_body_cap()` (`http_wire.hpp:516,606,654`). This is the backstop;
   (1)+(2) bound each section on its own.
4. Egress sets `max_header_bytes = min(64 KiB, effective_cap)` (a head larger than a tiny cap fails while it is being read, not
   after), `max_body_bytes = effective_cap`, therefore worst case read = `effective_cap + 2*min(64 KiB, effective_cap)`.

Gates (own slice, `tests/rt/test_http_*`): R1 a peer streaming `HTTP/1.1 100 Continue\r\n\r\n` forever ends after <= 8 interims
and the bytes read are bounded; R2 a head of 2 KiB with `max_header_bytes = 1 KiB` fails `header_too_large`; R3 trailer flood
(> budget) fails; R4 each section just under its own budget but the total over `max_response_bytes` fails `byte_cap_exceeded`;
R6 **[R4-m3]** unit rows: `max_body_bytes = UINT64_MAX` and `max_header_bytes = UINT64_MAX` derive a saturated total (no wrap), and a normal response still succeeds; R5 provider-plane defaults: a normal SSE stream and a `100 Continue` + 200 still succeed. **Mutants:** the parser reset without
counting (R1 fails); `max_response_bytes` counted at decode instead of at the `read_some` return (R4 fails); derivation from the raw `max_body_bytes` with plain `+` (R6 fails); egress not narrowing `max_header_bytes`
(R2-egress variant fails).

## 4. D1 -- the seam: stop, scope, deadline [R2-3]

```cpp
struct AdmissionKey { std::string tenant; std::string principal; std::string root_run; };   // [R4-1] STRUCTURED, never joined
struct EgressCallControl {                       // values only; none of them is authority (I2)
    std::stop_token stop;
    std::chrono::steady_clock::time_point deadline{};   // default (epoch) = unset
    AdmissionKey admission;                              // host-derived bucket labels (8.2); only narrows
};
concept NetEgressBackend = requires(T b, cap::NetOut const& g, NetEgressRequest const& r, EgressCallControl c) {
    { T::traits } -> std::convertible_to<NetEgressTraits const&>;
    { b.fetch(r, g, c) } -> std::same_as<task<result<NetEgressResponse>>>;
};
```
- A token and a time point can only make the callee stop sooner; `EffectContext` (carrying `capabilities`) is not passed to a
  conformer (I2). No default on `HostEgressProxy::fetch`: forgetting is a compile error.
- Callers pass `{ctx.cancellation, ctx.deadline, admission_of(ctx)}` (`read_content.hpp:299`, `extract_pdf_text.hpp:217`; `EffectContext::deadline`,
  `core/effect_context.hpp:55`, default-constructed = epoch = unset).
- Inside `fetch` (precedent `src/protocol/sse_stream_pump.hpp:203-205`, `core/replay_chat_client.hpp:151-153`): a `std::stop_source
  merged` is requested by the caller token AND by `rt::scope_stop_token()` (host `Runtime` shutdown requests the root token,
  `rt/runtime.hpp:430-437`; a `when_all` sibling failure requests the scope token). `merged` is what `rt::http_request` gets.
- `effective_total = min(total_timeout, deadline - now)` when `deadline` is set; an already-expired deadline fails
  `net.deadline_exceeded` before any resolve. The pure function `effective_total_timeout(configured, deadline, now)` is exposed
  for tests (clamp `<= 0` -> default **[R2-12]**).
- **[R4-1]** `admission_of(ctx)` = `{ctx.principal.tenant_id, ctx.principal.id, ctx.root_run_id.empty() ? ctx.run_id : ctx.root_run_id}`:
  all host-derived, never model-derived (the principal comes from the session or the ADR-061 bridge, `agent_session_core.cpp:945,951`;
  `root_run_id` is new, 8.4). It replaces round 3's `tenant + "/" + root_id()`, which collapsed every single-principal or
  anonymous deployment into one bucket (`make_embedded_principal`, `trust/principal.hpp:70`; an unset `Principal{}` has empty id
  and tenant, `agent_session_core.cpp:714`) and whose unescaped `/` let tenant `a/b`+root `c` collide with tenant `a`+root `b/c`.
- Call sites/fakes updated: 2 production, 3 fakes, ~17 direct test/live calls (`EgressCallControl{}` written explicitly).

## 5. D2 -- one gate, one code path [R2-6,8,9,10]

```cpp
struct GatedEgressTarget { std::string host; std::uint16_t port; bool https; };
[[nodiscard]] result<GatedEgressTarget> egress_gate(NetEgressRequest& req, cap::NetOut const&);  // pure, no I/O, in-place
```
Body = `net_egress_proxy.cpp:752-795` moved (C1, `parse_allowlist_entry`, C3 incl. `#ifndef AGENTENGINE_WITH_HTTPS`, C9
`reject_crlf` extended to NUL, C7), then **[R2-9]** *strip* every guest header named (case-insensitive) `Host`, `Content-Length`,
`Transfer-Encoding`, `Connection` in place (no body copy, **[R2-10]**), then `rt::http_wire::validate_request(...)` -- a new
validate-only function split out of `build_request` (`http_wire.hpp:197-258`), which `build_request` itself now calls first, so
there is one rule set and no body-copying validate pass.
- **[R4-m1] Request-body cap.** The gate refuses a body larger than `max_request_bytes` (host-set on the egress context, default
  16 MiB) as `resource` / `net.request_too_large`, before any copy beyond the single WIT one. No grant field capped the request
  body (C8 caps responses only), and round 3 showed the "bounded by `memory_bytes`" claim was false (the limiter is installed
  only when `memory_bytes > 0`, `wasm_backend.cpp:848-850`, and the copies are on the host heap). The WASM import checks
  `byte_list.of.list.size` against the same constant **before** `req.body.reserve` (`wasm_backend.cpp:540-548`), so an oversized
  guest body is never copied at all. Peak host memory per call is now `<= 3 * max_request_bytes` (2 after S3), times at most 64
  admitted calls. G17.
- **[R3-6]** After stripping, `egress_gate` refuses a non-empty body on GET, HEAD, TRACE and OPTIONS (`contract`,
  `net.invalid_request`): the CL.0 desync class (a body some origins ignore and read as a second request) was pre-existing in the
  sync path (it sent the body with a recomputed `Content-Length`). DELETE with a body stays allowed (some real APIs use it) -- named
  residual R13. A guest that sends raw chunk-encoded bytes with `Transfer-Encoding: chunked` gets them sent as an opaque body
  with the computed Content-Length (the framing always matches the bytes sent; the only effect is the guest corrupting its own
  data; one sentence in `wit/ae-tool.wit`).
- **[R3-9] Body copies.** `fetch` takes the request by value; the body is moved, not copied, down the chain: `egress_gate`
  (in place) -> `HttpRequest` (move) -> `build_request` (`http_wire.hpp:247-258`, builds the wire string from a `string_view`) ->
  the step-5 `std::vector<std::byte>` (`rt/http.hpp:~718`). Remaining copies, stated honestly: one from the WIT list into
  `req.body` (`wasm_backend.cpp:540-548`, unavoidable), one into the wire string, one into the write vector -- 3 live copies at the
  peak on the WASM edge (was 4-5), bounded by the guest's `memory_bytes`. Slice S2 moves the wire string into the write vector
  instead of copying it (2 copies).
- The proxy owns framing: Host is the allowlisted name (+ port when not default, RFC 9110 7.2), Content-Length is computed from
  the body, `Connection: close` is fixed. A guest-supplied Content-Length can no longer smuggle (it was forwarded unchecked
  before, `build_raw_request :404-412`); a guest `Host` can no longer address another virtual host. Documented in 008 / `wit/ae-tool.wit`.
- Flagged: a model URL with a space or control byte, which was sent raw, is now a clean `contract` error
  (`net.invalid_request` / `net.header_injection_rejected`). **[R2-9]**
- **No `resolver` field; one seam pair, test-only [R2-11/L][R3-5]:** `host_resolver` (`rt::HostResolver`; production default
  `ipv4_only_resolver(pal::resolve_host_blocking)`), `address_policy` (`rt::AddressPolicy`; production default
  `egress_address_policy`), `tls_ca_override`. **There is no seam on the production class: no friend, no setter.** They exist
  only on a SEPARATE type, `TestEgressProxy`, defined in `sandbox/testing/net_egress_proxy_testing.hpp`, which conforms to
  `NetEgressBackend` and calls the shared gate/`fetch` implementation, which takes the seams as parameters (production passes the
  defaults). **[R4-m4]** Those implementation functions are declared in `sandbox/detail/net_egress_impl.hpp`, which is covered by
  the SAME lint as the testing header (only `src/sandbox/`, `sandbox/testing/` and `tests/` may include it), so "the production *type*
  has no seam" is the accurate claim; the functions are not a seam reachable from ordinary host code. A deployer cannot reach a non-default seam from `HostEgressProxy`; to use one they must
  name `TestEgressProxy`. Honest scope: host code is trusted, so a deployer *including the testing header* is not an I2 break;
  the claim is only that the production type has no seam to flip (round 2 found the friend-function version was not a seal:
  any translation unit can define a friend). G15: compile-negative (no setter/friend on the production type) plus a lint that no
  file under `include/`, `src/` or `examples/` outside `sandbox/testing/`, `sandbox/detail/` and `src/sandbox/` includes either header. The WASM import's
  `HostEgressProxy const proxy;` stays default.
- **Shared-gate identity [R2-8]:** **[R3-10]** `TestEgressProxy` counts gate calls **per instance** (the shared `fetch` implementation takes an optional
  `GateObserver*`; production passes null; no process-global counter, so parallel or live rows cannot disturb it). G1 asserts
  `fetch` and `fetch_blocking` each call the gate exactly once, before the fake resolver's first call (the fake records the count it saw).

## 6. D3 -- `AddressPolicy` (unchanged by round 1; confirmed sound by the red-team)

```cpp
bool egress_address_policy(pal::IpAddress const& a) noexcept {
    if (a.family != pal::IpAddress::family_kind::v4) return false;   // fail closed on EVERY non-v4 (family, never decoded)
    return !is_blocked_address(be32(a.bytes[0], a.bytes[1], a.bytes[2], a.bytes[3]));
}
```
Refused by family: `::1`, `::`, `::ffff:0:0/96` (incl. `::ffff:127.0.0.1`, `::ffff:10.x`, public `::ffff:8.8.8.8`), `64:ff9b::/96`,
`2002::/16`, `fe80::/10` (+scope), `fc00::/7`, `ff00::/8`, Teredo, documentation, global unicast. The PAL keeps v6 as v6 and never
unmaps (`resolver_getaddrinfo.cpp:68-74`, confirmed by the red-team; pinned by a test). `ipv4_only_resolver(inner)` drops non-v4
and returns `not_found` when none remain, so AAAA-only stays `net.host_unresolvable` (transient). Numeric v6 literal hosts skip the
resolver (`rt/dns.hpp:228`) and hit the policy: `host_unresolvable` -> `address_blocked` (flagged, 9). `rt/dns.hpp`'s "test seam,
production leaves empty" comment is updated: production passes the v4 filter through that slot. v6 table = residual R2.
**[R2-6]** There is no synchronous resolve path any more, so the v6-to-`uint32` truncation hazard of a sync `VerifiedEndpoint`
does not exist.
**[R2-16] Inherited residual R9:** `is_blocked_address` (`:288`, table `:50-60`) is pre-existing, not widened here, and is NOT
complete: it omits 192.0.0.0/24, 198.18.0.0/15, the three documentation ranges, and the host's own public interface addresses
(hairpin). This ADR calls it "the existing table", not "the single table".

## 7. D4/D5 -- byte cap and deadline [R2-1,7]

- Byte cap: see D0(4). `effective_cap = min(granted.byte_cap.value_or(16 MiB), 16 MiB)`. Behaviour change vs `:461`'s head+body
  cap, now **for the WASM edge too**: a grant `byte_cap` N admits N body bytes plus heads sharing `min(64 KiB, N)`; chunk framing
  bytes count as body wire bytes; `net_bytes` narrowing (`narrow_by_resource_limit`, `net_egress_proxy.hpp:239`) means "response
  body wire bytes"; no cumulative accounting exists (verified), none added. A cap smaller than any real head fails
  `net.header_too_large`, as it failed `net.byte_cap_exceeded` before.
- Deadline: `HostEgressProxy::total_timeout` (host-set, default `kDefaultEgressTotalTimeout = 120 s`, `<= 0` clamps to the default;
  there is no unbounded mode) narrowed by the call's `deadline` (D1). `HttpOptions::total_timeout` is a reactor timer started before
  DNS (`rt/http.hpp:681-691`) and covers resolve + connect + TLS + body. `idle_timeout = sandbox::io_timeout_ms()`.
- **[R2-7/L][R3-1] The bound per caller [corrects round 2's overclaim for WASM]:** a tool caller is released by
  `min(total_timeout, ctx.deadline)`, `ctx.cancellation` and `rt::scope_stop_token()`. A WASM guest is released by
  `min(total_timeout, call_start + inst.limits.wall_ms, ctx.deadline)` and by `ctx.cancellation`, because the WASM edge now
  passes them (8.3). The sync exchange's unbounded HTTPS/DNS waits are gone with it; a DNS lookup already running still occupies
  a pool worker until `getaddrinfo` returns (R1) but no caller waits on it. The guest's own `wall_ms` is wasmtime epoch
  interruption (`wasm_backend.cpp:761,853`), checked only while guest code runs, never inside a host import -- which is why the
  host must pass the deadline in; otherwise the trap fires only after the import returns and `likely_timeout`
  (`:1001-1002`) mislabels it.

## 8. D6/D8 -- TLS, and the one egress I/O context

### 8.1 TLS [R2-2]
`ctx.tls = rt::tls_connector(...)` (under `AGENTENGINE_WITH_HTTPS`); `HttpRequest::host` = the allowlisted **name**, never the
address (`rt/http.hpp:704`). Without the option, `egress_gate` returns `net.scheme_unsupported` before any resolve; an empty
`ctx.tls` is a second refusal (`tls_unavailable`, same code).
**Class carry-through [R4-L: S2, no longer part of S1]:** `HttpTlsHandshake` (`rt/http.hpp:231`) gains `http_failure failure` (rt-local enum `{unspecified, transient,
policy, fatal}`), set in `rt/http_tls.hpp:~125` from `tls_error`: `certificate_rejected`, `handshake_failed` -> `policy`;
`setup_failed` -> `fatal`; `transport` -> `transient`. `HttpResponse` carries it as `failure`. The sync client already returned
`policy` for the first two and `fatal` for setup (`src/backends/tls_mbedtls/mbedtls_client_config.cpp:66-110`); a local mapping by
`http_error::tls_failed -> transient` (round-1 finding 2) would have downgraded a MITM/wrong certificate to retryable.
`egress_error` uses `failure` when set, else the code table in 9. The provider plane ignores `failure`: `http_error_to_error`
(`protocol/provider_chat_wire.hpp:72`) is unchanged, so this adds data to `HttpResponse` but changes no provider mapping.

### 8.2 The egress I/O context [R2-4,5/L]
Facts (verified): `~Runtime` joins the DNS pool (running `getaddrinfo`), the lanes and the reactor (`rt/runtime.hpp:450-453`); the
last release of a per-proxy holder would land on a host lane. A borrowed `Runtime*`'s `dns_pool()` is the provider plane's.
Decision: **one process-wide `EgressIoContext`**, created lazily once (`std::mutex` + `Runtime* p`; a failed construction leaves
`p` null so the next call retries), **intentionally never destroyed**: `new rt::Runtime(RuntimeConfig{.lanes = 2, .offload_workers = 1,
.dns_workers = 2})` stored in a static pointer; no destructor, no join on any engine thread, no static-destruction join (documented
leak, reachable so LSan stays quiet). It has its OWN reactor and its OWN DNS pool; egress never touches a borrowed Runtime's pool,
and there is no `Runtime*` parameter on `HostEgressProxy` at all (**removed**; only the context is used). Construction failure
(`std::system_error`, `bad_alloc`) -> `resource` / `net.egress_unavailable`. Not in a coroutine hot path: the context is fetched
before the first `co_await`.
- **Why lanes at all [disagreement with "no lanes", with evidence]; two of them [R4-m6]:** the sync edge needs a coroutine home that is not the blocked
  caller. Homeless waiters are refused by the reactor (`rt/reactor_await.hpp:8,37-39`); `block_on`/`Runtime::run`/`RootHandle::join`
  are refused on a lane or offload worker (`rt/lanes.hpp:209`, `rt/runtime.hpp:~385`, `rt/block_on.hpp:329`), and a WASM tool runs
  inline on whatever thread drives its `make_sync_invoke` task (`wasm_tool_bridge.hpp:~140`), i.e. a lane in production. The only
  non-blocking root launcher is `Runtime::spawn(task)` (`runtime.hpp:548-579`), which runs the root on a lane. The lanes are used
  only by sync-edge roots; async callers resume on their own strand (completions post to the caller's home), so they are idle
  for them. Two, not one, **[R4-m6]**: with 64 sync-edge roots each parsing up to 16 MiB on a single lane, every continuation
  (a delivered cancel included) would queue behind up to ~1 GiB of parsing; two lanes halve that and cost an idle thread. The
  real figure is a prove-step measurement (R12), not an argument.
- **Admission [R3-2][R4-1][R4-m5]: two levels under a global cap, structured keys, host-configurable.** Per ROOT RUN 8 (key
  `(tenant, principal, root_run_id)`), per PRINCIPAL 32 (key `(tenant, principal)`), global 64; `EgressLimits{per_run, per_principal,
  global, max_request_bytes}` is a host-set value on the context (atomics, adjustable at any time; host code, never model output).
  The key components only select which bucket to debit, so they can only narrow, exactly like the stop token (I2-neutral).
  - A host that never sets a principal (`Principal{}`: empty id and tenant) still gets per-root-run isolation; the principal level is
    **not applied to an empty principal** (it would be a hidden global 32 for every anonymous session). A host that uses ONE
    service principal for everything gets per-run isolation (8) under an effective global 32 -- it should raise
    `per_principal` (honest consequence, documented).
  - A child of `agent.spawn`, a background job, or a workflow node charges the ROOT run's bucket (8.4), so fan-out cannot multiply
    the quota. Two runs of one principal have separate run buckets, so one saturated run cannot starve its sibling run.
  - Implementation: `std::mutex` + `std::map<AdmissionTuple, std::uint32_t>` per level with **erase-when-zero** (a never-shrinking
    map would grow with every inbound principal); the RAII slot decrements and erases under the lock; keys are tuples of the
    three strings, never concatenated. Two atomic counters alone cannot implement per-key, as round 3 noted.
  - **Bounded wait [R4-m5]:** on exhaustion the call waits, up to 1 s capped by `control.deadline` and `stop`, retrying admission
    every 25 ms with `rt::sleep_for` on the egress reactor, before failing `resource` / `net.egress_busy` with the message
    "egress is busy; retry after a short delay" (so a model reading it does not spin). Transient saturation is then invisible.
  - **Residual R1:** many *distinct* root runs of *distinct* principals can still exhaust the global 64; blast radius = other
    principals' egress for up to `total_timeout`, never the provider plane (separate pool). The caps bound the otherwise unbounded
    DNS queue (`rt/offload.hpp:303-309`).

### 8.3 `fetch_blocking` [R2-6/L][R3-1,4]
`fetch_blocking(req, granted, control)` (the 2-arg form is removed; the WASM import supplies a control value) does not run the
gate itself; it builds `fetch(req, granted, control)` and runs it on the egress context: `RootHandle h = ctx->spawn(root)`, where
the root `co_await`s `fetch`, stores its `result` in a `shared_ptr<Slot>` and last-statement `count_down()`s a latch the WASM
thread waits on with a plain `std::latch`/`std::future` (NOT `block_on`, `Runtime::run` nor `RootHandle::join`, all refused on
lanes). **[R3-1]** `fetch_blocking` keeps `h` and registers a `std::stop_callback` on `control.stop` that calls
`h.request_stop()`; `fetch` merges `rt::scope_stop_token()` (the root's, on the egress runtime) so that request reaches the
in-flight operation.
**What the import supplies** (`wasm_backend.cpp:844-846`): `CallCapabilities` (`:384-386`) gains an `EgressCallControl egress`
member filled in `invoke_tool` (`:824`), which already holds `ctx`: `stop = ctx.cancellation`; `deadline = min(call_start +
inst.limits.wall_ms [when > 0], ctx.deadline [when set])`; `admission = admission_of(ctx)`. `cb_http_request` passes
`call->egress` and applies the request-body size check (5) before copying the guest list.
**[R3-4] The parked lane, stated.** The latch parks the host lane exactly as the sync exchange did; this is the hazard that
makes `block_on` refuse lanes (`lanes.hpp:209`), accepted because the blocking path parked it already. It is now bounded by
`min(total_timeout, wall_ms, ctx.deadline)` (7), and `RuntimeConfig::lane_stall_bound` watchdogs (`rt/runtime.hpp:96-110`,
report-only) will report it while it lasts. Nothing the root needs runs on the waiting thread: no lane deadlock. A `spawn`
failure -> `resource` / `net.egress_unavailable`. The WASM guest sees the same parser, byte cap, deadline, Host and codes as tool
callers; `rt::http`'s chunk/TE/CL/obs-fold strictness now applies to the WASM edge (flagged: a response the old client tolerated
now fails `net.protocol_error`-class, transient). `resolve_and_validate*` stay for Kata (`kata_backend.cpp:259`) and the relay
(`native_jail_handle_relay.cpp:361`) (R6).

### 8.4 `EffectContext::root_run_id` -- every place a run starts [R4-1]
`EffectContext` gains `std::string root_run_id`: **host-set only, never model-derived**, default empty (empty -> `admission_of` falls
back to `run_id`; both empty -> the run level is skipped, global/principal levels still apply). Rule: a *root* run's value is its own
`run_id`; every descendant carries the root's. Every place a run or an `EffectContext` is created, and how it is filled:

| Where | How `root_run_id` is filled |
|---|---|
| Session core, a run starts: `src/rt/agent_session_core.cpp:211` (`effect_context_.run_id = session_id_ + ":run:" + n`) -- reached by `start_run`, `resolve_interaction`, the MCP/A2A request path (ADR-061 Tier 3), quickstart sessions and eval trials alike | `effect_context_.root_run_id = inherited_root_run_id_.empty() ? run_id : inherited_root_run_id_`. `inherited_root_run_id_` is a session member, empty for a top-level session, set once for a child/node session (rows below). Two runs of one top-level session are sequential (I1) and each is its own root. |
| `agent.spawn` child: `perform_agent_spawn` builds `ChildSpawnRequest` (`rt/agent_spawn.hpp:~531-535`, struct at `rt/agent_spawn_child_run.hpp:72`) next to `child_request.principal = ctx.principal` | New field `ChildSpawnRequest::root_run_id`, set from `ctx.root_run_id`; `run_child_agent_session` passes it to the child session's `inherited_root_run_id_` before `start_run`. Nested spawns propagate it unchanged (the root, not the parent). |
| Background job: `rt/background_job_runner.hpp:625-640` builds `EffectContext ctx` from the allowlisted `job.spec` (`BackgroundJobOwner{session_id, run_id, turn_index}`, `:107-111`) | New field `BackgroundJobOwner::root_run_id`, filled by the session core when it submits (`agent_session_core.cpp:~880-893`, beside `owner_run_id`) from `effect_context_.root_run_id`; the runner copies `ctx.root_run_id = job.spec.owner.root_run_id` next to `ctx.run_id = job.spec.owner.run_id` (`:634`). |
| Workflow agent node: `agent_session_as_executor_body` (`rt/agent_workflow_executor.hpp:~101-135`) runs `session.start_run()` under the node's `ctx` | Before `start_run`, `session.set_inherited_root_run_id(ctx.root_run_id)`; the supervisor's per-node contexts (`workflow_supervisor.hpp:737-760`, a missing entry is a fresh `EffectContext{}`) have `root_run_id` stamped by the supervisor with the **workflow run's id** when empty (a workflow run id exists, `rt/workflow_run_state_record.hpp:404-416`), or inherited when the workflow is itself driven from a tool of an outer run. A top-level workflow's N agent nodes therefore share one bucket. Implementation must confirm the supervisor has the id at stamping time; if not, the host stamps it into `contexts[i]` and the gap is R14. |
| Workflow function / moderator node tool calls | They receive the node `ctx` directly: the stamped value. |
| Standing effects (`rt/standing_effect_registry.hpp:174`) | Not an `EffectContext` creator: they fire through background jobs (row 3). |
| Direct tool dispatch with no session (tests, `HostEgressProxy` called by host code) | Empty -> `run_id` fallback -> if also empty, run level skipped. |
| `restore_from_record` (`agent_session_core.cpp:790-802`) | `root_run_id` is persisted as an additive optional field in the same session record that already holds `principal_id`/`principal_tenant_id`/`run_counter` (`:790-801`); absent (old records) = the run's own id. `delegation_root` is NOT persisted today (the principal rebuilt at `:800` drops it, `principal.hpp:51-54`); this ADR does not change that and notes the consequence: a restored delegated child's principal-level bucket is its own id's. The record-schema/version check is an explicit S2 step; if the record type cannot take an additive field cleanly, R14 stays open. |

## 9. Error and audit codes [R2-2 rebuilt from the code]

`egress_error(HttpResponse const&)` is local to `net_egress_proxy.cpp` (`protocol/provider_chat_wire.hpp:72` is in `protocol/`,
not includable, and maps `malformed_response` to `contract`). Messages NEVER contain the resolved address or any topology **[R2-13]**.

| Today (sync) | class | Async source | class | WASM wit case |
|---|---|---|---|---|
| `net.ambiguous_grant`, `net.malformed_allowlist_entry`, `net.scheme_unsupported`, `net.method_not_allowed` | policy/contract | gate (+`tls_unavailable`) | same | host-not-allowlisted |
| `net.header_injection_rejected` | contract | gate / wire validate | contract | host-not-allowlisted |
| `net.invalid_request` (new) | contract | gate / wire validate (bad method, header name, target, GET/HEAD/TRACE/OPTIONS body) | contract | **[R3-9] network-error** (a malformed request; host-not-allowlisted would mislead guest authors) |
| `net.host_unresolvable` | transient | `resolve_failed` | transient | host-not-allowlisted |
| `net.address_blocked` | policy | `address_rejected` | policy | host-not-allowlisted |
| `net.connect_failed` (connect, send, recv, idle) | transient | `connect_failed`; `transport` (`net.connect_failed`) | transient | network-error |
| `net.connect_failed` ("timed out reading response") | transient | `net.idle_timeout` | transient | **add** timeout |
| `net.connect_failed` (recv failed) | transient | `net.stream_read_failed` (`http.hpp:201`, `http_tls.hpp:92`) | transient | network-error |
| `net.connect_failed` (EMFILE; none distinguished) | transient | `net.too_many_open_files` (`http.hpp:201`) | **resource** (flagged) | network-error |
| `net.tls_certificate_rejected` / `net.tls_handshake_failed` (`mbedtls_client_config.cpp:101-110`) | **policy** | same codes via `failure` | **policy** | network-error |
| `net.tls_setup_failed` (`:50-99`) | **fatal** | same via `failure` | **fatal** | network-error |
| `net.byte_cap_exceeded` | resource | `body_too_large` (`net.byte_cap_exceeded`) | resource | byte-cap-exceeded |
| head over cap (was `byte_cap_exceeded`) | resource | `net.header_too_large` | resource | **add** byte-cap-exceeded |
| `net.cancelled` | canceled | `canceled` | canceled | n/a |
| `net.protocol_error`, chunk/framing | transient | `net.protocol_error`, `net.framing_conflict`, `net.content_length_*`, `net.chunked_malformed`, `net.obs_fold_rejected`, `net.transfer_coding_unsupported` | **transient** (preserved) | network-error |
| `net.stream_truncated` | transient | `truncated` | transient | network-error |
| (none) | -- | `net.deadline_exceeded` | **resource** (I8 wall budget) | **add** timeout |
| (none) | -- | `net.egress_busy` (after the 1 s bounded wait; message carries a retry hint), `net.egress_unavailable`, **[R4-m1]** `net.request_too_large` | resource | network-error; `net.request_too_large` -> byte-cap-exceeded |
| (none) | -- | `net.no_address_policy`, `net.busy`, `net.consumer_stalled` (unreachable here) | policy / contract / resource | network-error |

Flagged class/code changes: numeric v6 literal `host_unresolvable` -> `address_blocked`; EMFILE transient -> resource;
`net.deadline_exceeded` new (resource); `net.idle_timeout` / `net.stream_read_failed` replace `net.connect_failed` codes; WASM `timeout`
becomes reachable. The streaming twins (`:667-743`) are untouched.

## 10. Attribution (I4) [R2-13]

No egress-specific audit record exists and `fetch` has no `EffectContext`. Verified: `ToolInvocationAudit` (`core/tool_pipeline.hpp:
187-207`) records call id, tool, ok, error code, result bytes, duration and principal for every tool call, so a tool-mediated fetch
is attributable at tool granularity; `run_event_kind` (`core/run_event.hpp`) has `sandbox_exec_*` but no network event. This ADR
therefore does **not** add one and **drops** the claim that `HttpResponse::address` is audit data (it is not recorded anywhere and
not returned in `NetEgressResponse`). Residual R10: a per-egress record (grant entry, resolved address, status, body bytes, code)
needs a new `run_event_kind` and a `ctx`-carrying seam; a follow-on ADR.

## 11. Gates (each names the mutant that must fail it) [R2-8,12,14 renamed/corrected]

W = Windows-provable; L = needs Linux (WSL2); H = needs `AGENTENGINE_WITH_HTTPS`. Tests extend `tests/sandbox/test_net_egress_proxy.cpp`.

| Gate | Claim | Mutant |
|---|---|---|
| G0 (W) | The D0 gates R1-R5 (3) | see 3 |
| G1 (W) **[R3-10]** | **Identity:** `TestEgressProxy` counts gate calls per instance; `fetch` and `fetch_blocking` each call the gate exactly once, before the first `host_resolver` call. **Fetch-level discriminators** (rows `rt::http` cannot reject): C1 zero/two entries, malformed entry, C7 method restriction yield the gate's codes and 0 resolver calls; passing rows exactly 1 | async path omits C1 / C7 / the malformed-entry check; gate bypassed on the sync entry; resolve before gate |
| G2 (W) | **Defence in depth, each layer mutated alone:** CRLF/NUL in method/path/header name/value and https-without-TLS rejected with equal codes by (a) `egress_gate` called directly and (b) `rt::http_request` called directly; through `fetch` both hold | mutant A: remove CRLF/NUL from `egress_gate` -> (a) fails, `fetch` still passes (layer b holds); mutant B: remove from `validate_request` -> (b) fails, `fetch` still passes; **both** removed -> `fetch` row fails |
| G2b (W) **[R2-9]** | Strip-and-recompute: guest `Content-Length: 5` with a 100-byte body -> server sees exactly `Content-Length: 100`; guest `Host: evil` -> server sees exactly one `Host` = allowlisted name; guest `Transfer-Encoding`/`Connection` absent/recomputed; bodiless POST -> `Content-Length: 0` | strip loop removed (server records duplicate Host / forged CL) |
| G3 (W) | Policy over every v4 range of the existing matrix + non-blocks (172.32, 8.8.8.8, 1.1.1.1); C5 literal encodings: **assert no connect** (`accept_count()==0` on a 127.0.0.1 listener) through an injected resolver (CI); the real-resolver variants are local/Linux only, never CI **[R2-14]** | policy skips `is_blocked_address`; 172.16/12 edge |
| G4 (W, L for `::1`) | Each v6 form of 6 injected via `host_resolver` -> `net.address_blocked`, listener `accept_count()==0`; **permit-all policy + a v6 answer** still never connects on either entry point (no truncation path exists) **[R2-6]**; PAL no-unmap pin | `egress_address_policy` returns true for v6 |
| G5 (W) | `ipv4_only_resolver`: AAAA-only -> `net.host_unresolvable` transient; A+AAAA connects over A | filter removed |
| G6 (W) | Positive controls: policy accepts 8.8.8.8 / 1.1.1.1 / 93.184.216.34; injected loopback + permit-all returns 200 + body; direct loopback connect succeeds outside the proxy while the default policy refuses it. A real public-v4 connect is live-network-labelled only (R7) | policy over-blocks; default permits loopback |
| G7 (W) **[R2-3][R3-1]** | Cancel: pre-fired stop -> `canceled`, 0 resolver calls; stop mid-body returns `canceled` < 1 s, socket closed; tool-level: recording fake sees `ctx.cancellation`, `ctx.deadline` and the admission key; **destroy the host `Runtime` while a fetch is parked -> `canceled` well under 1 s**; **`when_all` sibling failure cancels a parked fetch**; a 200 ms `deadline` with a slow peer -> `net.deadline_exceeded` at ~200 ms. **WASM row:** a plugin with `wall_ms = 2000` parked in `http-request` against a silent allowlisted peer returns within ~2 s plus slack (not 120 s); a `ctx.cancellation` stop requested mid-fetch returns `canceled` in < 1 s; destroying the host Runtime while it is parked completes in < 1 s | stop not forwarded; `scope_stop_token()` not merged; deadline ignored; tool passes `{}`; **`fetch_blocking`/`cb_http_request` passes `{}`** (WASM row hangs to `total_timeout`); `request_stop()` not wired to the `RootHandle` |
| G8 (W) | Trickle: 1 byte/100 ms for 10 s, `total_timeout` 500 ms, idle 5 s -> `net.deadline_exceeded` < 2 s on `fetch` AND on `fetch_blocking` (same bound, HTTPS variant under H); `effective_total_timeout()` unit rows: 0 and negative -> default, deadline narrows, expired deadline expired **[R2-12]** | `total_timeout` 0 downstream; clamp removed |
| G9 (W) **[R3-7]** | Byte cap: body = cap passes, cap+1 fails mid-stream; no cap -> 16 MiB ceiling; head > `min(64 KiB, cap)` -> `net.header_too_large`; 1xx flood and trailer flood through egress stay <= `cap + 2*min(64 KiB, cap)` **counted on the client** (a test hook on the transport's `read_some` return, never at the peer, whose kernel-buffer slack makes peer counts flaky); `cap = 0` allows 0 bytes (not unlimited); memory-capped with `tests/support/memory_cap.hpp` + watchdog | `max_body_bytes` default; `max_header_bytes` not narrowed; D0 mutants; derived 0 treated as unlimited |
| G10 (W) | 3xx returned unmodified, `Location` target `accept_count()==0` on both entry points | redirect-following |
| G11 (W+H) **[R2-2]** | Cert for name A, allowlist host B: `net.tls_certificate_rejected`, class **policy**, through BOTH `fetch` and `fetch_blocking`, no response; matching name with CA override -> 200 (control); broken CA bundle -> `net.tls_setup_failed` **fatal**; IP-literal https allowlist entry row; fake resolver sees exactly the allowlisted string | `tls_failed` mapped transient; `failure` not carried; host replaced by address text |
| G12 (W) | Error table (9): each reachable row produced by a scripted peer/resolver on both entry points with the tabled class and code; WASM mapping test adds `net.invalid_request`, `net.header_too_large`, `net.deadline_exceeded`/`net.idle_timeout` -> `timeout` | a row in the wrong class; mapping unextended |
| G13 (W, L) **[R2-4,5][R3-3,8][R4-1,m5]** | (a) **timing:** drop the `fetch` frame while a fake resolver sleeps 5 s -> the dropping thread returns in < 100 ms; (b) 20 gate-rejected calls start zero threads and never touch the context; (d) 64 concurrent first calls create exactly one context (TSan, L); (e) global cap: the 65th concurrent call waits up to 1 s then `net.egress_busy` (not instantly), and succeeds if a slot frees within the wait; (f) construction failure injected -> `net.egress_unavailable`, next call retries; (g) ASan, L: a fetch whose per-attempt timer fires as its frame is destroyed, after its host Runtime is gone -> no use-after-free; **(h) admission, run level:** two runs of the SAME principal -- run A saturates its 8 with trickling peers, run B (same principal, different `root_run_id`) still fetches to a fast peer; a child of A (`agent.spawn`, a background job, a workflow node) charges A's bucket; **(i) principal level:** one principal's 4 runs x 8 hit 32 and the 33rd fails while another principal still succeeds; an empty principal is not subject to the principal level but its sessions are isolated per run; **(j) key structure:** tenant `a/b` + root `c` and tenant `a` + root `b/c` get separate buckets; the maps are empty again after every slot is released (erase-when-zero) | per-proxy holder / destructor join; lazy init without the mutex; no cap; **key on principal only** (h fails: run B starved); **joined-string key** (j fails); no erase-when-zero (j fails); child keyed on its own run id (h child row fails); wait removed (e fails). Old G13c dropped [R3-8] |
| G14 (W) | `static_assert(NetEgressBackend<HostEgressProxy>)`; the 3-arg fakes compile; a fake with the old 2-arg `fetch` fails the `static_assert` (negative compile row) | concept left at 2 args |
| G15 (W) **[R2-11][R3-5]** | Compile-negative: the production `HostEgressProxy` has no setter, friend or seam member; `TestEgressProxy` exists only in the testing header and satisfies `NetEgressBackend`; lint: no file under `include/`, `src/` or `examples/` outside `sandbox/testing/` includes that header | a public `address_policy` field or a friend |
| G16 (W) **[R3-6]** | GET, HEAD, TRACE and OPTIONS with a non-empty body -> `contract` `net.invalid_request`, 0 resolver calls, nothing on the wire; POST/PUT/PATCH/DELETE with a body pass (DELETE recorded as R13) | gate check removed (server sees a body on GET) |
| G17 (W) **[R4-m1]** | A request body of 17 MiB -> `resource` / `net.request_too_large`, 0 resolver calls, through `fetch`, `fetch_blocking` and the WASM import (the import refuses before `reserve`: peak RSS stays flat, memory-capped); a 16 MiB body passes; `max_request_bytes` host-configurable | gate check removed; import checks after the copy |

Evidence rule: each mutant is applied for real, the named gate observed to fail, then reverted by restoring a scratchpad copy (never
`git checkout --`). The full suite is compared with a stash-isolated baseline before any commit.

## 12. Alternatives rejected

`EffectContext` instead of a control value (ambient reach); a default `stop` (silent bug shape); keep `resolver` / a sync resolve
path (two implementations, the v6 truncation hazard); a hand-written v6 table now; per-proxy-object or per-tool-class runtime
holders (a join on a host lane; N runtimes); a borrowed `Runtime*` and its DNS pool (cross-plane starvation); a sync-edge deadline
check inside the blocking exchange (cannot bound TLS record reads or `getaddrinfo`); rejecting managed headers (breaks WASM guests
whose HTTP library sets `Host`/`Content-Length`, and the error was misleading); provider-style "empty policy = accept all".

## 13. Residuals

- **R1 [R4-1]** Many distinct root runs of distinct principals can still exhaust the global 64 slots and the 2 egress DNS workers and slow other principals' egress for up to `total_timeout`; never the provider plane. A single root run is held to 8, a principal to 32. A running lookup cannot be interrupted.
- **R2** IPv6 egress unsupported (fail closed); a v6 table needs its own ADR and matrix.
- **R3** (round 1) *removed*. **R4** *removed* (one byte-cap meaning everywhere, changed from head+body).
- **R5 [R3-3]** The egress context is intentionally leaked (2 lanes + 1 offload + 2 DNS workers + the reactor thread, for the life of the process, created on first fetch). Consequences, stated: (a) its threads are alive during static destruction, so an abandoned `getaddrinfo` can still be running inside libc/NSS while `exit()` tears statics down (its completion touches only the pool, `rt/offload.hpp:464`, and the function-local `chain_detail::Registry`); (b) a host that embeds AgentEngine in a DLL and `FreeLibrary`s it would leave threads running in unmapped code -- the engine is a static library today, so no current build does this; (c) the egress reactor outlives every host home by construction; a late completion posted to a destroyed host Runtime's strand is queued and never run (a leaked frame, not a use-after-free; G13g proves it).
- **R6** `resolve_and_validate*` remain for Kata and the Python relay with their own blocking connect; E6 owns the relay.
- **R7** A public-v4 end-to-end connect is not CI-provable (live-network label only).
- **R8** One exchange per connection; no pooling.
- **R9** The v4 blocklist omits 192.0.0.0/24, 198.18.0.0/15, documentation ranges, own public addresses (6).
- **R10** No per-egress audit record (10).
- **R11** `perform_http[s]_exchange*` remain for `ProviderHttpClient`; their removal is that client's slice.
- **R12 [R4-m6]** The sync edge occupies the 2 egress lanes only for continuation slices; a flood of sync-edge calls serialises their parsing on them (bounded by the 64 admission cap). The queueing figure (worst case 64 x 16 MiB of parsing over 2 lanes) is measured in the prove step, not asserted here.
  serialises their parsing on it (bounded by the 64 admission cap).

## 14. Implementation slices [R4-L, R4-2] (in order; each independently shippable)

**S1 -- D0, `rt::http` limits (provider plane included; nothing else).** Shared head budget, `max_interim_responses = 8`, request-level
`max_response_bytes` counted at the `read_some` return and derived with saturating arithmetic from `effective_body_cap()`, `optional`
sentinel. Touches `rt/http.hpp`, `rt/http_wire.hpp` only; **no change to the provider plane's error mapping and no TLS change**
(the TLS `failure` carry-through moved to S2). Provider plane defaults: 8 interims; derived total 16 MiB + 128 KiB.
*Gates:* G0 = D0 R1 (100-Continue flood <= 8, bytes bounded), R2 (head > budget), R3 (trailer flood), R4 (sections under budget, total
over), R5 (normal SSE stream and `100 Continue` + 200 still pass; the provider-plane suite re-run unchanged), R6 (saturating
derivation); the D0 mutants; the cap-0 row at the `rt` level.

**S2 -- the whole egress move: both entry points together.** (a) `EffectContext::root_run_id` plumbing (8.4, including the session record
field) and `admission_of`; (b) `EgressIoContext` (leaked 2-lane Runtime, own DNS pool), two-level admission with the bounded wait,
`EgressLimits`; (c) `EgressCallControl` and the 3-arg concept, async `fetch` with merged stop and deadline, tools updated; (d)
`egress_gate` + `validate_request` + **managed-header stripping + GET/HEAD/TRACE/OPTIONS body refusal + `max_request_bytes`** (so the
gate never rejects what the WASM guests legitimately send); (e) `ipv4_only_resolver`, `egress_address_policy`,
`effective_total_timeout`, `egress_error` (table 9) with the TLS `failure` carry-through; (f) `TestEgressProxy`, the testing and
`detail` headers, the `resolver` field deleted and ~12 tests rewritten; (g) **the WASM edge via spawn + latch**, `RootHandle::
request_stop` wired (spawn first, then register the stop callback), `CallCapabilities::egress`, the import's request-size check,
the wit mapping extension; (h) the old blocking exchange removed from the proxy path. *Gates:* G1, G2 (both layers), G2b, G3, G4,
G5, G6, G7 (tool rows and the WASM row), G8 (both entry points, plain and under H), G9, G10, G11 (both entry points), G12, G13 a-j,
G14, G15, G16, G17. Because the gate, the strip and both entry points move together, no intermediate state rejects a request the
old WASM path accepted or loses the sync edge's test seam.

**S3 -- remainder.** The write-vector move (the wire string is moved into the step-5 `std::vector<std::byte>` instead of copied:
body peak copies 3 -> 2), the `wit/ae-tool.wit` note (managed headers are stripped and recomputed; GET/HEAD/TRACE/OPTIONS bodies
are refused; a raw chunk-encoded body is sent as an opaque body). *Gates:* G17's peak-memory row re-measured at 2 copies; the
existing WASM suite; G2b re-run.
