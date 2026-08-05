# SSRF and DNS-rebinding defense for a host-mediated egress proxy

Dated 2026-08-05. Feeds `decisions/ADR-011-first-party-egress-proxy.md` (M2 Phase F task F1,
`008-Sandbox-and-Isolation.md` §10 Q3). Scope: what a first-party egress proxy needs to get right to
actually block the abuse cases 008 §7 names ("DNS-rebinding around an egress allowlist", "SSRF to
link-local metadata endpoints") rather than merely assert it does.

## 1. Allowlists beat denylists, but neither is the actual mechanism here

OWASP's own SSRF prevention guidance treats denylisting (blocking RFC 1918 + loopback ranges) as
insufficient as a *standalone* control, because string/regex-based denylists are bypassable by DNS
rebinding, IPv6 representations, decimal IP encoding, and URL-encoded characters — and recommends
allowlist validation of scheme/host/port as the structurally stronger boundary
([OWASP Server Side Request Forgery Prevention Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Server_Side_Request_Forgery_Prevention_Cheat_Sheet.html)).

This project's design already has the allowlist (007 §3's `NetOut` capability, "parameterized, not
boolean... `NetOut` without an allowlist is not a capability, it is a hole"). What OWASP's finding
actually implies for this ADR is narrower and more mechanical: **the allowlist decides which
hostname a guest may address; it says nothing about which numeric address that hostname is actually
allowed to resolve to.** A guest granted `NetOut<"api.search.example">` has passed the allowlist
check the moment it names that host — DNS-rebinding and SSRF-via-redirect are both attacks on the
gap between "the hostname passed the allowlist" and "the byte stream is going to the host the
allowlist entry meant."

## 2. DNS rebinding: the mechanism and the mitigation this ADR adopts

Classic DNS rebinding: an attacker's own DNS record returns a public (allowlist-passing) IP on the
resolution a naive validator uses to check the target, then returns an internal/private IP on the
resolution the actual `connect()` call performs — a TOCTOU race between validate-time and
connect-time DNS answers, exploitable because most HTTP client stacks resolve the hostname twice
(once implicitly for any pre-flight check, once again inside `connect()`/`getaddrinfo` at request
time). Discussion and a defensive framing consistent with this: resolve the domain once, validate
the resolved address against the allowlist/blocklist, and then connect to *that resolved IP address
directly* rather than letting the underlying HTTP/socket stack re-resolve the hostname at connect
time.

**This is the exact mechanism `HostEgressProxy` uses**: `resolve_and_validate(host)` performs exactly
one `getaddrinfo` call, validates the returned address, and the subsequent `connect()` targets that
literal numeric address — never the hostname a second time. There is no second resolution for an
attacker's rebind to land in, closing the race by construction rather than by a narrower time-window
mitigation (e.g. "re-check within N ms of connect," which only shrinks the race, doesn't remove it).

## 3. Metadata endpoints are a named instance of the same class, not a special case

Cloud IMDS endpoints (`169.254.169.254` on AWS/GCP/Azure) sit inside the link-local range
(`169.254.0.0/16`) and, absent a session-token requirement (IMDSv2-style hardening), return
unauthenticated credentials over plain HTTP to anything that can reach them — the reason SSRF-to-IMDS
chains are a named, high-severity finding pattern in cloud environments
([Resecurity: SSRF to AWS Metadata Exposure](https://www.resecurity.com/blog/article/ssrf-to-aws-metadata-exposure-how-attackers-steal-cloud-credentials)).
This proxy has no IMDSv2-style session-token concept to depend on (it isn't a cloud provider), so the
only correct posture is what 008 §7/§10 already mandate: link-local (`169.254.0.0/16`, which is a
superset containing `169.254.169.254`) is blocked unconditionally, never a per-deployment opt-in.

## 4. Why the blocked-range check runs on the resolved binary address, never a string

A large, well-documented bypass class ("IPFuscation") represents an IP address in a non-canonical
form — decimal (`2130706433`), octal (`0177.0000.0000.0001`), hex (`0x7f000001`), or mixed/truncated
forms — that a naive string-matching or regex-based filter fails to recognize as the address it
blocks, even though the OS resolver/`inet_pton` accepts and connects to it identically
([Filters and Bypasses — Rare IPv4 Formats for SSRF](https://dominicbreuker.com/post/filters_bypasses_rare_ipv4_formats_for_ssrf/)).
The documented fix is to normalize through `inet_pton`/equivalent before filtering, never filter the
input string directly.

A second, separately-documented instance of the same root cause is the IPv4-mapped IPv6 address
(`::ffff:127.0.0.1` / `::ffff:7f00:1`) bypassing a filter that only recognizes IPv4-form loopback/
private addresses — a real, disclosed vulnerability class, not a theoretical one
([GHSA-vrcj-hv2q-c58m: SSRF protection bypass via IPv4-mapped IPv6 address normalization](https://github.com/twentyhq/twenty/security/advisories/GHSA-vrcj-hv2q-c58m)).

**Both classes are closed by construction here, not by enumerating encodings**: `is_blocked_address`
never runs against a string at all — it runs against the 32-bit `in_addr` `getaddrinfo` (or
`inet_pton` for an IP-literal allowlist entry) already produced, which is the canonical binary form
regardless of how the original text was spelled. The IPv6-form bypass is moot for a different reason
(§5): this proxy's connect path is IPv4-only (matching the vendored PAL's own current locator
limitation, `third_party/quark/pal/*/net.hpp`), so `getaddrinfo` is called with `ai_family = AF_INET`
and an IPv6-only answer is a resolution failure, not a silently-accepted alternate encoding of an
address this proxy would otherwise have blocked.

## 5. What this proxy deliberately does not attempt this milestone

- **HTTPS/TLS.** No TLS or HTTP client library is vendored anywhere in this repository; rolling a TLS
  implementation from scratch for this ADR would be exactly the kind of unaudited-crypto risk the
  project avoids elsewhere (008 §10 Q3's own resolution text draws this same line for the proxy
  itself: "it doesn't need a vetted third-party crypto library the way `SecureTransport` does" — but
  that reasoning applies to the *allowlist/DNS* mechanism, not to a bespoke TLS stack, which this ADR
  does not attempt to build). `https://` allowlist/request entries are rejected with a clear,
  structured error. A follow-up ADR is needed once a TLS client exists to extend `HostEgressProxy` to
  `https`.
- **IPv6 egress.** Matches the vendored PAL's own IPv4-only locator (`quark::pal::tcp_connect` takes a
  32-bit host-order address). An IPv6-only host fails closed with a clear "could not resolve an
  IPv4 address for this host" error — an honest capability gap, not a silent narrowing of the
  blocklist's coverage.
- **Automatic redirect-following.** A 3xx response is returned to the caller as-is; the proxy does not
  re-issue the request against the `Location` target. This isn't a missing feature so much as a
  deliberate scope cut: transparently following redirects across the allowlist boundary is itself a
  well-known SSRF sub-class (the redirect target was never itself checked against the grant), and not
  following redirects at all removes the class entirely rather than requiring a second, harder-to-get-
  right re-validation-per-hop mechanism. A caller that needs to follow a redirect issues a new,
  separately-allowlist-checked request.
