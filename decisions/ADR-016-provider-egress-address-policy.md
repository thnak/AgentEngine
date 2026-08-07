# ADR-016 — Host-initiated provider egress: address policy and transport

**Status:** Accepted — 2026-08-07
**Supersedes:** the Milestone 5 Phase C implementation note in
`include/agentengine/sandbox/provider_http_client.hpp` that reused `resolve_and_validate` on the
provider path "as defense-in-depth even though the threat model differs".
**Does not change:** ADR-011 (first-party egress proxy) claims C1–C10, which are about the WASM
**guest** egress path (`HostEgressProxy::fetch`) and remain enforced there unchanged.
**Relates to:** 004-Model-Provider-Plane.md §3, ADR-013 (HTTPS egress TLS client).

## 1. Context

`sandbox::perform_provider_https_exchange` is the host-initiated HTTP client that ChatClient backends
(`OpenAIChatClient`, `AnthropicChatClient`) call to reach an inference API. Milestone 5 Phase C built
it by reusing two primitives from the guest egress proxy:

- `resolve_and_validate` — resolve once, then refuse the result if `is_blocked_address` says it falls
  in loopback / RFC 1918 / link-local / CGNAT / multicast / reserved / unspecified.
- `perform_https_exchange` — ADR-013's TLS transport.

Both reuses were reasonable defaults at the time. Both turn out to be wrong for this path, and the
combination makes an endpoint class that 004 §3 **explicitly names as a target** unreachable:

> 004 §3, on the OpenAI-compatible backend: "the default/widest-reach backend (OpenAI, gateways,
> **vLLM/llama.cpp/Ollama-style local servers**, most vendor compat endpoints)".

A local llama.cpp server is on `127.0.0.1` and speaks plain HTTP. Under Phase C's implementation it
failed twice over: the address was refused before a socket was opened, and even with the address
solved there was no non-TLS transport for a server that has no certificate. Binding such a server to
`0.0.0.0` does not help — that only makes it reachable on an RFC 1918 LAN address, which is equally
blocked.

## 2. Decision — address policy

**The blocked-range table is guest-path-only. Host-initiated provider calls resolve without it.**

`resolve_and_validate` is split into two functions over one shared resolution implementation, so the
two cannot drift:

| Function | Blocked-range filter | Used by |
|---|---|---|
| `resolve_and_validate(host, port)` | **yes** | `HostEgressProxy::fetch` — the WASM guest egress path (ADR-011) |
| `resolve_host(host, port)` | no | `perform_provider_https_exchange` — host-initiated provider calls (004 §3) |

Both keep ADR-011's resolve-once-connect-to-a-verified-literal discipline (claim C6, the DNS-rebinding
defense): exactly one resolution attempt, and the connect step takes a `VerifiedEndpoint` value, never
a hostname. That mechanism is transport- and threat-model-independent and is **not** relaxed.

### Rationale

The blocked-range table (ADR-011 claims C4/C5) is an **SSRF** defense. SSRF requires an attacker who
influences the destination. On the two paths that influence differs completely:

- **Guest path.** The host comes from a `cap::NetOut` grant that a sandboxed WASM guest asked to use,
  in a system where 006 §7's taint boundary exists precisely because guest-reachable values may be
  derived from model output. A guest steering a request at `169.254.169.254` or an internal service
  is the textbook attack. The table is load-bearing.
- **Provider path.** The host is a field of the `ChatClient` the deployment itself constructed from
  its own configuration. It is not guest-supplied, and by **I3** it can never be derived from model
  output — model output is data, never authority, and nothing on this call path reads it. There is no
  attacker on the other side of the check.

A check with no attacker to stop is not defence-in-depth; it is a false positive. Here it had a
concrete cost — it rejected the ordinary case (a local or in-cluster inference server) while stopping
nothing, and it pushed tests toward injecting a fake resolver to work around a defense that was never
protecting anything on that path, which is strictly worse for confidence than not having it.

### What is explicitly *not* claimed

This does not make the provider path safe against a **compromised configuration**. An operator who
can set the provider host can already point it anywhere; that was true before this ADR (the table
only constrained *which* wrong host, not whether one could be set) and remains true. Provider host
configuration is an operator-trust boundary, governed by 018, not by an address table.

## 3. Decision — transport

**TLS remains the default. A plaintext transport is available opt-in, never inferred.**

`ProviderTransport { tls, plaintext_http }` is a new parameter, appended last on
`perform_provider_https_exchange` and on both ChatClient constructors, defaulting to `tls`.

- It is a **named enumerator, not a bool**, so a construction site reads as
  `ProviderTransport::plaintext_http` and a reviewer sees the choice without opening a header.
- It is **never inferred** from a URL scheme and never probed from the endpoint. This is 004 §3's
  "capabilities are declared, not probed" rule applied to the transport.
- `ca_bundle_pem_override` is ignored under `plaintext_http` rather than being a construction error,
  so a caller comparing the two transports need not restructure its arguments.

### The trade, stated plainly

Under `plaintext_http` the request still carries the provider credential — `Authorization: Bearer …`
for the OpenAI-compatible backend, `x-api-key` for Anthropic — and that header **crosses the wire in
clear**. On loopback this is uncontroversial: an attacker who can read loopback traffic already owns
the process. On any real network it is a credential disclosure. That is why it is opt-in, non-default,
and named at the call site.

Both transports are otherwise identical by construction: `perform_http_exchange` and
`perform_https_exchange` share request building, the byte-cap-enforced read loop (ADR-011 claim C8),
the no-redirect-following posture (C10), and `stop_token` cancellation. The branch in
`perform_provider_https_exchange` is one line.

### Chunked responses: verified, not assumed

An earlier draft of this ADR claimed plaintext streaming was unsupported, on the grounds that
`perform_http_exchange` is documented "Content-Length-framed only — no chunked transfer-encoding
support". **That claim was wrong, and `test_llamacpp_live_e2e.cpp` LC-6 falsified it against a real
server before this ADR was accepted.** The accurate statement is narrower:

- Neither `perform_http_exchange` nor `perform_https_exchange` *decodes* chunked framing. Both, when
  a response carries no `Content-Length`, read until the peer closes (the raw-request builder always
  sends `Connection: close`, so that is a reliable terminator) and return the still-framed bytes.
- The chunked *decoding* lives one layer up, in the backend: `openai::detail::decoded_response_body`
  and `parse_streaming_response_into_updates` both branch on `response_is_chunked`.

So chunked framing is handled identically on both transports, because the layer that handles it sits
above the transport split. Non-streaming chunked responses and SSE streaming both work over
plaintext. LC-6 now asserts that positively.

The general limitation remains for a *direct* caller of `perform_http_exchange`/
`perform_https_exchange` that does its own parsing: it must decode chunked framing itself. That is
ADR-011's original scope and is unchanged here.

## 4. Falsifiable gates

| # | Claim | What would falsify it |
|---|---|---|
| G1 | The guest path still refuses every blocked range. | `test_net_egress_proxy.cpp` accepting any address in `kBlockedRanges` through `resolve_and_validate` or `HostEgressProxy::fetch`. |
| G2 | The provider path reaches a private/loopback address. | `resolve_host("127.0.0.1", p)` failing, or a real `OpenAIChatClient` failing to complete against a loopback server for an address-policy reason. |
| G3 | The two resolvers cannot drift. | A resolution behaviour (literal fast path, IPv4-only, one attempt, candidate order) observable in one and not the other. |
| G4 | TLS is still the default. | A default-constructed provider exchange or ChatClient completing against a plaintext server. |
| G5 | Plaintext is genuinely usable for the motivating case. | `test_llamacpp_live_e2e.cpp` failing to complete a real chat/tool/structured-output exchange against a real llama.cpp server. |

G1 is a **positive control** and the most important one here: this ADR only holds if relaxing the
provider path demonstrably did not relax the guest path. `test_provider_egress_address_policy.cpp`
asserts both halves against the same address in the same run.

## 5. Consequences

- Local and in-cluster inference servers work through the real ChatClient, with no test-only seam.
- The injectable `resolver` seam survives, with a narrower purpose: binding an arbitrary `Host:` name
  to an ephemeral port without a DNS lookup. It is no longer needed merely to reach loopback.
- One new asymmetry to remember: two resolvers, chosen by who initiated the call. The table in §2 and
  the header comments on both functions are the record.
