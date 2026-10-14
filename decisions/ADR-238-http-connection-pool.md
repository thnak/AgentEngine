# ADR-238: Reusing HTTP/1.1 connections on rt::http (keep-alive pool)

**Status: Proposed (2026-10-14). Design only; nothing is built.** Red-teamed once against the code (§7): no
FATAL findings, 8 MAJOR and 3 MINOR, all addressed in this revision.

This ADR is the performance follow-on that ADR-237 §6.3 left out of scope: "Connection reuse ... is in scope
as a performance follow-on, not a correctness requirement of this ADR". It is also the follow-up that ADR-237
review finding 6 pointed to. Quickstart sessions can now share one host `rt::Runtime` through `.runtime()`
(commit a402870). Sessions on the same runtime that call the same provider are exactly the case where reuse
pays.

## 1. The question

Every `rt::http` exchange opens a new connection and closes it afterwards. `http_wire::build_request` always
sends `Connection: close` (`rt/http_wire.hpp:196,260`). For every request, `open_stream` resolves the name,
filters the address through the policy, connects, and runs a full TLS handshake (`rt/http.hpp`, steps 3-4).
That handshake re-parses the CA bundle each time (ADR-237 §6.3). A tool loop making 20 model calls to one
provider therefore pays for 20 TCP connections and 20 TLS handshakes.

The question is whether a connection can carry a second exchange, and how, without weakening anything the
current one-connection-per-exchange design guarantees.

## 2. What reuse must not break

1. **One session's bytes never reach another session (I2, I4).**
   - With `Connection: close`, nothing outlives the exchange. A reused connection does.
   - If an exchange stops part-way and its connection goes back into the pool, the next exchange reads the
     end of the previous response as its own.
   - That is response desync, and across sessions it is a data leak.
2. **The address and TLS checks still hold.**
   - ADR-011 resolves the name once and connects only to an address the `AddressPolicy` accepts.
   - ADR-013 verifies the certificate against the original host name, using the configured CA bundle.
   - A reused connection must have passed the same checks the new request would run. Otherwise a request
     under a strict policy could ride a connection opened under an accept-everything one.
3. **Nothing is shared by default (I2).** Sharing is passed in explicitly. This matches finding 6: the host
   passes the runtime, and clients never discover each other.
4. **No new automatic retry of a POST that may have been applied.** Every model call is a billed POST. RFC 9110
   §9.2.2 (retrieved 2026-10-14) says: "A client SHOULD NOT automatically retry a request with a
   non-idempotent method unless it has some means to know that the request semantics are actually
   idempotent ... or some means to detect that the original request was never applied". It calls retrying a
   POST "if the underlying transport connection closed before any part of a response is received,
   particularly if an idle persistent connection was used" a "riskier approach".
5. **Shutdown stays bounded (ADR-237 §4.5 rule 4).** Idle connections hold no task, but they do hold sockets.

## 3. Design

### 3.1 Who owns the pool

`rt::HttpConnectionPool` is owned by `rt::Runtime` (`Runtime::http_pool()`). `HttpContext` gains one field,
`std::shared_ptr<HttpConnectionPool> pool`. It is a `shared_ptr` so that a body reader that finishes after the
runtime has begun shutting down still holds a live pool to return its connection to (§3.7).

- **Null pool:** exactly today's behaviour. `Connection: close` is sent and the connection is never reused.
- **Non-null pool:** the request may take a connection from the pool and may return its own.

The provider path turns pooling on through one setting:

- `provider_wire::make_http_context` sets `pool` only when `ProviderReactorOptions::pool_connections` is true.
- That setting defaults to false. It flips to true only after the §5 gates run in CI and G4 shows the
  stale-connection rate is negligible.
- A client's private runtime (the null `runtime` case) gets a pool that only that client uses.
- Sharing across clients needs the host to pass a shared runtime (`.runtime()`), and stays within the rules
  of §3.2.

Nothing is process-global. Two runtimes never share a connection.

**The guest egress path never gets a pool.** `HostEgressProxy` (ADR-011) carries requests to hosts the model
chose, under policies that differ per grant. Its contexts are always built with a null pool. This is written
down so that "reuse is cheap" is not applied there next.

### 3.2 Pool key and trust domain

A connection can be reused only for a request whose key equals the connection's key in every field:

| Field | Why |
|---|---|
| scheme | http and https |
| host, as given, lowercased | The TLS hostname was verified against this name (ADR-013). |
| port | The **effective** port: `req.port`, or 443/80 when it is 0 (`rt/http.hpp:666`). |
| trust domain | See below. |

The policy, the resolver and the TLS connector are all `std::function` (`rt/dns.hpp:84,99`,
`rt/http.hpp:237`), and two of those cannot be compared. So the context carries an explicit `HttpTrustDomain`,
an opaque value copied with the context. There are three kinds.

**`HttpTrustDomain::unique()`** is the default: a fresh id.
- It is minted **once per provider client**, in the client's constructor, and stored in
  `ProviderReactorOptions::trust_domain`.
- It cannot be minted in `make_http_context`. That function builds a new `HttpContext` on every call
  (`src/protocol/sse_stream_pump.hpp:42-53`, called per request from `openai/chat_client.cpp:510,533` and
  `anthropic/chat_client.cpp:508,529`). An id minted there would never match, and nothing would ever be reused.
- The result: one client reuses its own connections and no one else's.

**`HttpTrustDomain::provider_default(ca_bundle_pem_override)`** is a canonical id. Two provider clients share
it when all of these hold:
- their policy is the accept-all provider policy (ADR-016), i.e. empty;
- their resolver is the system one, exactly `sandbox::resolve_host`;
- no `rt::HostResolver` test seam was given;
- their CA override is the same.

This is decided **in the client constructor**, not in `make_http_context`. Both clients always fill
`reactor_.resolver` from their own resolver (`openai/chat_client.hpp:482-484`,
`anthropic/chat_client.hpp:512`), so by the time a context is built the resolver is never empty and "is this
the system resolver" cannot be answered there. The constructor checks it with `resolver.target<...>()`. Any
host policy, custom resolver, test seam or other CA override falls back to `unique()`.

**`HttpTrustDomain::named(std::string)`** lets host code have two custom-policy clients share. By naming the
domain, the host asserts the policies are equivalent. The name is host code, never model output (I3).

**The policy is checked again at checkout.**
- Before a pooled connection is used, the request's own `AddressPolicy` is called on that connection's
  verified peer address.
- If the policy refuses, the connection is closed and the request connects fresh through the normal path.
- So even a misnamed domain cannot send a request to an address its own policy rejects. This covers the
  policy only; resolver and CA differences are covered by the key.

### 3.3 Checkout

The request takes the most recently returned idle connection for its key (LIFO, so stale connections age out
at the back). It removes that connection from the pool **under the lock**. The checks below then run with the
lock released, so two requests can never hold the same connection.

A connection is discarded rather than used if any of these is true:

- **It has been idle too long.** The limit is `HttpPoolOptions::max_idle`, default 15 s (§6 explains why this
  is only a guess for now).
- **`TcpStream::peek_idle()` does not report `idle`.** On an idle HTTP/1.1 connection, any readable state
  means the server closed it, reset it, or sent bytes nobody asked for.
- **`HttpTransport::has_buffered_input()` is true.** A socket-level probe cannot see bytes already pulled
  into the TLS layer.
- **The policy re-check (§3.2) refuses its address.**

If nothing usable is left, the request connects through today's path, unchanged.

`peek_idle()` is a **new pal primitive**, and this ADR adds it.
- `pal::TcpStream` today only has the asynchronous `start_wait_readable` (`pal/reactor_tcp.hpp:166-168`).
  Cancelling an in-flight operation leaves the stream unusable, because the backend closes it
  (`pal/reactor_tcp.hpp:22-27`). So "start a wait, then cancel it" cannot be the probe.
- `peek_idle()` is a synchronous, non-blocking one-byte `recv(MSG_PEEK)`: `MSG_DONTWAIT` on Linux, and on
  Windows a non-blocking `recv` with `MSG_PEEK`. That is legal only while no overlapped read is outstanding,
  which is true of an idle pooled socket.
- It returns one of `idle`, `readable`, `closed` or `error`.

`has_buffered_input()` is new too:
- For TLS it is `mbedtls_ssl_get_bytes_avail()` plus `TlsStream`'s buffered ciphertext (`rt/tls.hpp:20-24`).
- For plain HTTP it is always false.

### 3.4 When a connection goes back to the pool

A connection is returned **only** when every rule below holds. Anything else closes it, as today.

1. **The response's end is exactly known.**
   - The framing was `content_length` or `chunked`, and the decoder reached `complete()` with **no** surplus
     bytes past the end. A response framed `until_close` (`rt/http_wire.hpp:407`) is never pooled.
   - Today `BodyDecoder::feed` silently ignores bytes past the end (`rt/http_wire.hpp:497-512`). With
     `Content-Length: 0` the decoder is already complete in its constructor, so bytes that arrived with the
     head are dropped without being counted (`rt/http.hpp:476-482`).
   - The decoder therefore gains a `surplus()` count, and both the head's leftover bytes and every later feed
     go through it. Any surplus closes the connection.
   - `has_buffered_input()` (§3.3) must also be false.
2. **No `close` connection option** in the request or the response. RFC 9112 §9.6: a client that receives one
   "MUST cease sending requests on that connection".
3. **HTTP/1.1, and not 101.** An HTTP/1.0 response is not pooled.
4. **The exchange ended cleanly.**
   - It was not cancelled, it hit neither the idle nor the total deadline, and the consumer read the stream
     to its end. **A stream the consumer drops part-way closes its connection.** Constraint 1 rests on this
     rule.
   - The test is `decoder.complete()`, **not** the reader's end status. `HttpBodyReader::close()` finishes
     with `end`, the same status as a natural end (`rt/http.hpp:536-542`).
5. **The whole request was written.**
6. **The pool has room:** `max_idle_per_key` (default 4) and `max_idle_total` (default 64).
7. **The response was 2xx, or the request had no body.** A server may answer 4xx before reading the whole
   request body and still keep the connection open. Its parser could then read the rest of that body as the
   start of the next request.
8. **The consumer-stall timer is confirmed dead.**
   - `StallTimer` closes the socket from the reactor thread through a `weak_ptr` (`rt/http.hpp:328-345`), and
     `disarm_stall` only posts a cancel (`:430-433`).
   - If the connection went back to the pool before that cancel landed, a timer that was already firing
     could close the socket under the next session's request.
   - So the return waits for the stall timer's own completion. If that completion reports `canceled`, the
     connection is pooled. If it reports `completed` (the timer fired), the connection is closed.
   - As a second line, each checkout carries a lease generation that the timer checks before it closes
     anything.

Rules 1 and 4 together mean a connection is pooled only when the engine knows exactly where the last response
ended. Ambiguous framing is rejected before any of this runs: Content-Length together with Transfer-Encoding
is `ambiguous_framing` (`rt/http_wire.hpp:18,462`).

### 3.5 Failures on a reused connection

A reused connection can fail before any byte of the response arrives. There are two cases, and they are not
the same.

**The request was not fully written:** the write failed, or the peer reset during it.
- A server cannot apply a request whose body it never fully received. RFC 9112 §9.3 says it must read the
  whole body or close the connection.
- That is the "means to detect that the original request was never applied" that RFC 9110 §9.2.2 asks for.
- So the transport retries **once**, on a fresh connection that was never pooled, and counts the retry.

**The request was fully written, then EOF or a reset before any response byte.**
- The server may have applied it, so the transport does **not** retry.
- It fails with `net.pooled_connection_lost`, class `transient`.

What happens next depends on how the session reaches the model.

**Direct route.**
- `should_retry_stream` retries only `any_update_seen`, `net.stream_truncated` and `net.stream_read_failed`
  (`src/rt/agent_session_core.cpp:1153-1173`). Its retry count defaults to 0
  (`rt/agent_session_core.hpp:552-559`).
- This ADR adds `net.pooled_connection_lost` to that list. With the default count of 0, nothing is retried
  and the run fails as transient.
- That is a new failure users can see. It is why the pooling default only flips after G4 has measured how
  often it happens.

**Gateway route.**
- `ModelCallGateway` retries any `transient` failure before it commits (`core/model_call_gateway.hpp:57-62,378`).
- The idempotency key is not yet read by any real backend adapter (`:203`).
- So this case **is** re-sent, and may be billed twice.
- That is not new: a fresh, unpooled connection reset after a full write gives the same transient today and
  is retried the same way. Pooling only makes it more frequent. The probe, `max_idle` and G4's counter exist
  to bound that frequency, and §6 discloses it.

### 3.6 Wire change

`build_request` takes a `keep_alive` flag.
- **false:** sends `Connection: close`, byte-identical to today.
- **true:** sends no `Connection` header; HTTP/1.1 defaults to a persistent connection.

A caller-supplied `Connection` header is still rejected (`rt/http_wire.hpp:30`).

### 3.7 Lifetime, shutdown, locking, audit

**Shutdown.** Idle connections are plain sockets owned by the pool; no task holds them. `~Runtime` does three
things, in this order:
1. sets the pool's `closed` flag, **before** `lanes_.stop()`;
2. closes every idle connection;
3. only then destroys the reactor.

Lanes still drain after the root count reaches 0 (`rt/runtime.hpp:446-452`), so a body reader can finish
late. A return to a closed pool closes the connection instead of keeping it.

**Active connections.** A connection in use belongs to its exchange. The exchange is a runtime root, so the
existing shutdown sweep cancels it, and a cancelled exchange closes its connection (rule 4).

**Locking.** The pool's mutex is held only while a connection is taken or returned. It is never held across an
await, because the coroutine can resume on another thread.

**Counters.** The pool counts:
- reuses;
- discards, by reason;
- retries of unwritten requests;
- `pooled_connection_lost` failures;
- late returns to a closed pool;
- evictions.

§5 measures these counters.

**Attribution (I4).** Each connection gets an id. `HttpResponse` carries that id and a `reused` flag, so an
audit record can show that two exchanges, possibly from two sessions, used one connection.

## 4. Alternatives considered

- **A global pool per process.** Rejected: it is ambient shared state, which constraint 3 and I2 forbid.
- **A pool owned by each client.** Simpler. But sessions sharing a runtime, the case finding 6 enabled, would
  never share. The `unique()` domain keeps per-client reuse as the default, and named or `provider_default`
  domains layer sharing on top.
- **Retrying a lost connection inside the transport, as browsers do.** Rejected for requests that were fully
  written. It is RFC 9110 §9.2.2's "riskier approach", and here it would be a second, hidden retry of a
  billed POST. Retrying only requests that were provably never applied is kept (§3.5).
- **HTTP/2 multiplexing.** It needs a new protocol stack, its own security review and its own ADR. Out of
  scope.
- **TLS session resumption, or caching the parsed CA bundle per trust domain.** Independent of pooling, with
  no desync risk, and still saves part of the cost. Worth doing as a separate change; not in this ADR.

## 5. Promotion gates

Each gate names the mutant it must catch. Gates run on loopback, using the `BurstServer`-style servers
already in `tests/core/chat/` and `tests/rt/`.

A test seam turns the checkout probe off. Every desync mutant is also run with the probe off, so the probe is
never the only thing that catches it.

**G1 Reuse happens, through the real client.**
- Test: two sequential `chat()` calls on one provider client reach a counting server over **one** accepted
  connection.
- It deliberately goes through `chat()` rather than a hand-built context, so a per-call trust domain
  (red-team finding 2) fails it.
- Mutants: "never return a connection", and "mint the domain in `make_http_context`". Each shows two
  accepts.

**G2 No desync after a dropped stream (constraint 1).**
- Session A opens a stream and stops reading part-way.
- The server **holds back A's remaining bytes** and sends them only once B's request has arrived, so the
  probe sees an idle socket.
- Session B, on the same runtime and in the same trust domain, makes a request. B must receive exactly its
  own response, and the server must see it arrive on a **new** connection.
- Both ways A can stop are tested: dropping the reader (`~BodyState`) and calling `HttpBodyReader::close()`.
- Mutants: "pool after a consumer stop", and "key rule 4 on the end status". In either case B reads A's
  bytes, which shows up as a body that does not match.

**G3 The trust domain is enforced (constraint 2).**
- Two clients with different `unique()` domains never share a connection. Mutant: "drop the domain from the
  key".
- The resolver returns 127.0.0.1 for one name and 127.0.0.2 for another. A `named` domain is shared by two
  contexts with different policies: one accepts both addresses, the other refuses 127.0.0.1. A pooled
  127.0.0.1 connection must never be used by the second context. Mutant: "skip the checkout re-check".

**G4 Stale connections (constraint 4).**
- **Fully written:** the server reads the whole second request, then closes as soon as it has it.
  - Expected: `net.pooled_connection_lost`, class `transient`, and on the direct route the server counts the
    POST exactly once.
  - Mutant: "transport retry after a full write" makes the server count two.
- **Not fully written:** the server resets during the request body.
  - Expected: one retry on a fresh connection, and the request succeeds.
  - Mutant: "no retry" makes it fail.
- **Gateway route:** a third run asserts the documented re-send, and the counter records it.

**G5 Framing and close rules.** The server keeps its socket open in every case, so EOF cannot give the
answer away. Each of these must leave the connection closed:
- an `until_close` response;
- a `Connection: close` response;
- an HTTP/1.0 response;
- surplus bytes after the end, including `Content-Length: 0` followed by extra bytes;
- a 4xx answer to a request that had a body;
- a TLS response with bytes still held inside the TLS layer.

Mutants: drop each rule in turn.

**G6 Late return after shutdown.**
- A body reader finishes after `~Runtime` has begun. Its connection must be closed, not kept, and the
  late-return counter must read 1.
- Mutant: "no `closed` flag", so the connection is kept and the leak check fails.
- Socket-after-reactor safety on its own is already guaranteed by pal (`pal/reactor_tcp.hpp:30-34`), so it
  cannot serve as this gate.
- Runs under ASan in CI's ASan jobs.

**G6b Stall timer.**
- A's consumer-stall timer is set to fire just as A's body completes. B then checks the connection out.
- A's timer must not close B's socket.
- Mutant: "pool on `disarm_stall`".

**G7 The benefit is real.**
- A bench runs 20 sequential HTTPS model-shaped calls against a local TLS server, with and without the pool,
  and also reports the handshake count.
- **Threshold: wall time must drop by at least 30%, or this ADR is closed as not worth its risk.**

## 6. Open points and residuals

- **The `max_idle` default.** It is not tied to any provider's real keep-alive timeout, because none has been
  measured. Before G7 sets the default, the keep-alive timeouts of the providers in use (OpenAI, Anthropic,
  OpenRouter, the owner's llama.cpp endpoint) are measured and written up in a dated `docs/research/` note.
- **`named` domains rest on the host's word.** The checkout re-check bounds the address. A resolver or CA
  difference under one name is a host error the engine does not detect. This is the ADR-070 seam: host
  opt-in, host code only, and it narrows nothing.
- **Billing on the gateway route.** A reused connection lost after a full write is re-sent (§3.5). That can
  double-bill, just as a fresh connection reset at the same point can today; pooling makes it more frequent.
  G4's counter measures it. The real fix is backends reading the idempotency key, which needs its own ADR.
- **A new visible failure on the direct route.** With `stream_retries` at its default of 0, a lost pooled
  connection fails the run as transient. Pooling stays off by default until G4 shows the rate is negligible.

## 7. Red-team

**Round 1, 2026-10-14.** One adversarial reviewer worked against the code. Result: no FATAL findings, 8 MAJOR
and 3 MINOR. Every file citation in the first draft was correct. The wrong parts were claims about how the
code behaves. All findings are addressed above.

| # | Sev | Finding | Fix |
|---|---|---|---|
| 1 | MAJOR | pal has no zero-timeout probe, and cancelling an async wait closes the stream. | §3.3: new `peek_idle()`; the connection is taken out of the pool before it is probed. |
| 2 | MAJOR | A `unique()` id minted per context never matches, because the context is rebuilt on every call. | §3.2: minted once per client; G1 goes through `chat()`. |
| 3 | MAJOR | "Is this the system resolver" cannot be answered in `make_http_context`, because the resolver is always filled in. | §3.2: decided in the client constructor. |
| 4 | MAJOR | "Handled as today" was false: the direct route does not retry failures before the head, and the gateway re-sends them. | §3.5: split into not-written and written cases, each route stated; §6 discloses the billing risk. |
| 5 | MAJOR | Surplus bytes were invisible: the decoder drops them, and TLS buffers can hold them. | §3.4 rule 1 adds `surplus()`; §3.3 adds `has_buffered_input()`. |
| 6 | MAJOR | The stall timer could close a socket after it was reused. | §3.4 rule 8; gate G6b. |
| 7 | MAJOR | A connection could be returned late to a pool shutting down, and G6 could not fail. | §3.1 `shared_ptr`; §3.7 `closed` flag; G6 rewritten. |
| 8 | MAJOR | The probe masked the G2 and G5 mutants, and `close()` ends with the same status as a natural end. | Probe-off seam; server holds back A's bytes; rule 4 keys on `complete()`. |
| 9 | MINOR | G3 could not work on a single loopback address. | Two loopback addresses. |
| 10 | MINOR | A 4xx can arrive before the server has read the whole request body. | §3.4 rule 7. |
| 11 | MINOR | G7 had no threshold; G4 depended on timing; the key used the raw port; the guest path and I4 were not covered. | §§3.1, 3.2, 3.7 and 5. |

The reviewer also confirmed four things as correct:
- ambiguous framing is rejected before pooling;
- 1xx and 101 responses are handled;
- HEAD, 204 and 304 responses are framed `none`, so they are never pooled;
- nothing derived from model output reaches the pool key or the policy (I3).
