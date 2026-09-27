# ADR-206 — The OpenAI and Anthropic chat clients: non-template code compiled once in `src/`, shared HTTP plumbing written once

- **Status**: **Proposed — design + implementation + proof (2026-09-27); red team round 1 (same day): no BLOCKER or
  MAJOR, 2 MINOR, both fixed (§8).**
- **Date**: 2026-09-27
- **Origin**: GitHub issue #120 (S6), the structure audit after issue #115 closed.
- **Touches**: `include/agentengine/protocol/{openai,anthropic}/chat_client.hpp` (declarations),
  `src/protocol/{openai,anthropic}/chat_client.cpp` (new), `include/agentengine/protocol/provider_chat_wire.hpp` +
  `src/protocol/provider_chat_wire.cpp` (new, shared), `src/protocol/sse_stream_pump.hpp` (new, private),
  `CMakeLists.txt` (`agentengine_provider_http_client` sources), `tools/layers.toml`.

## 1. The question

`OpenAIChatClient<Store>` and `AnthropicChatClient<Store>` are templates only on the secret store. Everything
else in their headers — the request translation, the response and SSE parsers, the streaming accumulators and
the detached stream worker, 1,500 body lines in 36 `inline` functions and 9 member functions — is ordinary
non-template code, and `core/session_builder.hpp` includes both headers. Every file that includes the session
builder or either client compiled and optimized all of it again (#120 counted 49 and 21 including files).

The two headers also carried the same code twice: the stream worker's push loop and its five terminal-error
rules, the HTTP status → `error` mapping, and `chat()`'s exchange tail were copies differing only in the vendor
name.

Can the bodies be compiled once, and the copies be one, without changing what either client does?

## 2. Designs considered

- **A. Move the non-template bodies to `src/`, verbatim, and write the shared plumbing once (chosen).**
- **B. Put the new sources in their own library.** Every consumer already links `agentengine::provider_http_client`
  (the clients call its exchange functions). A separate library would need every consumer's CMake changed, or a
  link cycle back to `provider_http_client` so that GNU ld sees the exchange symbols after the client objects.
  Adding three sources to the existing library needs neither.
- **C. A type-erased accumulator interface, so the shared pump is not a template.** It adds a virtual call per SSE
  fragment for no gain: the pump has exactly two instantiations, both in `src/protocol/`. A template in a private
  header next to its callers costs nothing and is visible to nothing else.
- **D. Leave it.** Every HTTPS build keeps paying for the bodies in each including file, and the two copies of
  the stream rules can drift (they already differed in the order of one `starts_with` test).

## 3. Decision

Design A, in two steps.

**Step 1 — verbatim move.** A script (the ADR-200 tooling, generalized to free functions and to a class inside a
namespace) moved every non-template, non-`constexpr` `inline` function and every `StreamingUpdateAccumulator`
member whose body has 6 or more lines: 36 free functions and 9 members, 1,500 body lines. The headers keep every
declaration with its comment and default arguments, the structs, the `constexpr` constants, the constructors and
the helpers under 6 lines, and the two client templates. OpenAI 1,262 → 548 lines,
Anthropic 1,311 → 570.

**Step 2 — one copy of what was duplicated.**

- `provider_wire::http_status_error(vendor, status, body)` (`protocol/provider_chat_wire.hpp`) is the old mapping
  with the vendor name as a parameter. `openai::detail::map_http_status_error` and
  `anthropic::detail::map_http_status_error` keep their names and signatures and forward to it (the embedder and
  the tests call them).
- `provider_wire::exchange_json(vendor, host, port, req, resolver, ca, transport)` is `chat()`'s old tail: the
  blocking POST, the transport error forwarded, a non-2xx mapped, the body parsed. Both `chat()` bodies call it.
- `provider_wire::pump_sse_stream<Accumulator>(vendor, model, host, port, req, producer, resolver, ca, transport,
  stop)` (`src/protocol/sse_stream_pump.hpp`, private) is the old worker from the `on_body` lambda to the final
  `close()`. Each `run_stream_worker` now builds its vendor's body and request, fails the producer if the body
  cannot be built, and calls the pump. Declarations and signatures of both workers are unchanged.
- Both `detail::Resolver` aliases name `provider_wire::Resolver` (the same `std::function` type as before).

**Link.** The three new sources are compiled into `agentengine_provider_http_client`. No consumer changes.

## 4. Behaviour — what is and is not the same

The claim is that no observable behaviour changes. What differs, precisely:

1. **Step 1** changes linkage only: the moved functions are no longer `inline`, so a caller in another file
   cannot inline them, and they are compiled with the library's flags. None is on a per-token path that a caller
   inlined across files: the per-fragment work was already inside the worker. No moved body has a `static` local,
   `thread_local`, `assert`, `#if`, `__FILE__` or `source_location`, and neither header has an `#if` apart from
   its file-wide `AGENTENGINE_WITH_HTTPS` guard, which the library and every consumer see identically (it is a
   `PUBLIC` definition of `agentengine_net_egress_proxy`).
2. **`http_status_error`** builds `"<vendor> http status <N>"` and `"<vendor>.http_<N>"` from `std::string(vendor)`
   where the copies had the literal; the bytes are the same (§5, a differential over every status 0–999).
3. **`exchange_json`** is the old tail statement for statement; the parse result is returned instead of being
   tested and unwrapped by the caller, which then tests it — one `if` moved.
4. **The pump**, against the two old workers:
   - `looks_like_sse` tests `data:`, `event:`, `:`; the Anthropic copy tested `event:` first. `||` over pure
     `starts_with` calls: same value.
   - The producer id is `std::string(vendor) + ":" + model`, where each copy wrote `"openai:" + model` /
     `"anthropic:" + model`: the same string.
   - `if (acc) { for (… acc->finish()) … }` lost its `if`: the line before returns when `!acc`, so the test was
     always true.
   - The producer is passed by reference from `run_stream_worker`, which still owns it by value, so it is
     destroyed at the same point as before (the end of the worker).
5. **Security.** The ADR-173/191 system-channel fence (`translate_message`, `split_system_messages`,
   `neutralize_outbound_text` callers) moved verbatim in step 1, unchanged. The API key still reaches the network
   only inside the `NetEgressRequest` built by each vendor's unchanged `build_http_request`, now passed to the
   pump by `const&`; nothing new logs, stores or copies it. Credential resolution stays in the templates' `chat()`
   and `chat_stream()` (004 §1), untouched.

## 5. Evidence

- **Moved verbatim (step 1).** A normalized line diff (comments, blank lines and whitespace dropped) between each
  old header and the new header plus its `.cpp`, taken before step 2: OpenAI 25 lines only in the old file and 71
  only in the new ones, Anthropic 30 and 87, and every one is a signature (the old definition line, the new
  declaration, the definition without `inline` / default arguments / `[[nodiscard]]`), a class-qualified member
  name, or the `.cpp`'s `#include` and namespace lines. No body line differs.
- **Status mapping.** `tests/core/chat/test_provider_chat_wire_equivalence.cpp`: both wrappers against verbatim
  copies of both old functions (checked byte-identical to `origin/main` by script), every status 0–999 × 27 bodies
  (27,000 pairs per backend), class, message and code: 0 differences. Self-control: the Anthropic wrapper against
  the OpenAI oracle differs on all 27,000; two one-field controls (401 reclassified, 500's message changed) are
  each counted on exactly the 27 changed pairs.
- **Pump.** The existing real-transport tests drive both backends' `chat_stream()` through it:
  `test_stream_retry_real_transport` (head-then-cut, cut mid-stream, 4xx, clean completion, for OpenAI and
  Anthropic), `test_chat_client_stream_incremental` (drip-fed SSE, both), `test_chat_client_stream_cancellation_
  bounded`, `test_openai_chat_client_live`, `test_anthropic_chat_client_live`. They did not cover the
  exchange's non-2xx branch in `chat()` or the producer id the pump stamps, so the new test adds, through the real
  clients and a loopback plaintext server, for both backends: `chat()` on a 401 with a JSON error document
  (policy, the document's message, `<vendor>.http_401`) and on a 503 with a non-JSON body (transient,
  `"<vendor> http status 503"`); `chat_stream()` on a 429 (transient, `<vendor>.http_429`, nothing pushed); and a
  streamed reasoning trace stamped `"<vendor>:<model>"`.
- **Builds.** MSVC 14.51, `dev` preset with `AGENTENGINE_WITH_HTTPS=ON` (`/O2` library, `/Od` tests), `/W4 /WX`,
  all 961 targets: no warnings. The three new sources under g++-14 `-O3 -DNDEBUG -Wall -Wextra -Werror`: clean.
  Linux: g++-14 Release (`-O2 -DNDEBUG`, `-Werror`), `AGENTENGINE_WITH_HTTPS=ON`, the 11 provider test targets built
  and run (the new test, both translation tests, both `*_chat_client_live` tests, the four streaming tests, the
  taint fence, the cross-backend parity): 11/11 pass.
- **Tests.** The same MSVC build, full non-live suite: 380/380 pass (2 skipped as always, the
  `*_no_process_creation` checks; the 11 Docker-daemon tests and `test_external_skill_discovery` excluded, as in
  ADR-200 §5). The count includes the HTTPS-only tests, which a default build does not have.
  `python tools/layering_lint.py` and `--self-test`: OK.
- **Live, against DeepSeek** (`test_openrouter_live_e2e`, host `api.deepseek.com`, model `deepseek-flash`), once per
  wire format. Chat Completions (`/v1`): OR-OAI-1 to 4 and 6 to 8 pass, including a real streamed SSE response through
  the shared pump and a real wrong-key 401 through `exchange_json` classified `policy` (OR-OAI-7). Messages API
  (DeepSeek's Anthropic-compatible `/anthropic/v1`): OR-ANT-1 to 4 and 6 to 8 pass, including real named-event
  streaming, tool use, thinking on and off, and a wrong-key 401 classified `policy`. What fails is the provider, not
  the change: DeepSeek accepts but does not honour structured-output formats on either API (OR-OAI-5, OR-ANT-5: the
  reply is not the requested JSON), and OR-PARITY-1 needs both endpoints in one run, which one prefix cannot give.
  `test_openai_chat_client_openrouter_live_e2e` names OpenAI models and needs OpenRouter itself; not run.

### 5a. Positive controls

Each control breaks one line of the shared code, rebuilds, and runs the provider tests. Every one is caught, on
both backends:

| Control | Caught by |
|---|---|
| C1: the pump closes cleanly when a 2xx head arrives with no body (the pre-ADR-019 bug) | `test_stream_retry_real_transport`: "a 200 head followed by a cut before ANY body byte is a truncation and is retried" (OpenAI) and its Anthropic twin |
| C2: the pump stamps `"x:<model>"` instead of `"<vendor>:<model>"` | the new test's reasoning case, OpenAI and Anthropic |
| C3: `exchange_json` skips its non-2xx check | the new test's 401 and 503 `chat()` cases, both backends (4 failures) |
| C4: `http_status_error` no longer classes 401 as policy | the differential (both backends) and the 401 `chat()` case (both backends) |

Before the new transport cases existed, C2 and C3 failed no test. A first version of C1 disabled the `!acc`
branch outright; the next line then dereferenced the empty `optional` and the test segfaulted. That proves
nothing about the branch, so the recorded C1 replaces the failure with `close()` instead.

## 6. Measurements

Single translation units, MSVC 14.51, compile commands from the HTTPS build, compiled serially, each twice with the
faster run kept, same machine and session. "Before" is the same command with both client headers from `main`. CPU is
the compiler process's total CPU time. Two flag sets: the `dev` preset's (tests at `/Od`) and the same commands at
`/O2 /Ob2 /DNDEBUG` (how the `release` preset builds tests).

| Translation unit | dev: wall / CPU before → after | `/O2`: wall / CPU before → after |
|---|---|---|
| `tests/protocol/openai/test_openai_chat_client_translation.cpp` | 2.3 / 2.4 → 2.2 / 2.3 s | 3.9 / 8.8 → 3.4 / 6.5 s (CPU −26 %) |
| `tests/core/context/test_session_builder.cpp` (both clients, via the session builder) | 4.4 / 4.9 → 4.2 / 4.7 s | 7.6 / 22.5 → 7.0 / 19.6 s (−13 %) |
| `examples/30_session_builder_quickstart_live.cpp` | 3.5 / 3.7 → 3.3 / 3.5 s | 5.8 / 15.0 → 5.0 / 12.2 s (−19 %) |
| `tests/core/json/test_json_value.cpp` (control: neither client) | 1.1 / 1.0 → 1.1 / 1.0 s | 1.6 / 2.5 → 1.6 / 2.5 s |

The new sources cost 7.2 s (OpenAI), 6.7 s (Anthropic) and 2.6 s (shared) of CPU, once per build.

What this buys, stated plainly: under `/O2`, about 2–3 CPU-s per including file, roughly 100–150 CPU-s across the
~50 includers of an optimized HTTPS build, less the 16.5 s paid once. At dev flags the saving is about 5 % per file,
because `/Od` never optimized the inline bodies to begin with. The larger gains are elsewhere: an edit to any moved
body (the parsers, the accumulators, the fence call sites) recompiles one file instead of every includer, and the
stream rules, status mapping and exchange tail now have one copy. None of it shows in CI, which builds no HTTPS
code (§7).

## 7. Residuals

- **No CI job compiles this code** (closed 2026-09-27: `.github/workflows/ci.yml` now configures the Linux leg and
  the MSVC ASan leg with `AGENTENGINE_WITH_HTTPS=ON`). Both headers are wrapped in `#ifdef AGENTENGINE_WITH_HTTPS`, and no CI leg
  configures with it on (MSVC Release, clang-cl, gcc-14 and the fuzz leg all use the default `OFF`). This predates
  the change: the clients, their translation tests and every live test have never been built by CI. This change is
  proven by local MSVC and g++-14 builds only (§5). Adding an HTTPS leg (it fetches mbedTLS and the CA bundle) is
  the natural follow-up; it would also make the saving in §6 show up in CI, which today builds none of it.
- The headers still include everything they did, because including files rely on them for transitive includes
  (the same residual as ADR-199 §7 and ADR-200 §7).
- `protocol/qdrant/vector_index.hpp` has its own status mapping, reading a different error shape (`status.error`),
  and its own exchange tail. It is not a chat client and not part of S6; it could use `exchange_json` with a
  mapping parameter later.
- As in ADR-200 §8: a host `.cpp` could define a moved function itself. Host code is trusted; outside the I2/I3
  model.

## 8. Red team round 1 (2026-09-27)

An independent pass, with its own scripts, and a positive control for every probe that came back clean:

- **Verbatim.** Its extractor paired all 45 moved definitions with their `origin/main` originals by name and
  parameter list: 32 byte-identical, 9 identical after the uniform 4-space de-indent of class members, and only
  `map_http_status_error` and `run_stream_worker` different in each file (step 2). All 45 header declarations match
  the old signatures (`[[nodiscard]]`, defaults, `noexcept`, `const`) but for `inline`; nothing is defined twice,
  and a census of old lines lost only signature, brace and step-2 lines. Controls: a one-letter change in
  `role_to_wire`, one in Anthropic's `items_from_block`, and a dropped default on `build_request_body` were each flagged.
- **Name lookup.** No moved body uses a namespace-scope name declared after it in the old header (the only hits were
  parameter/member names), out-of-line return types are qualified, no nested type shadows a namespace one; no
  `static`, `thread_local`, `assert`, `#if` or `constexpr` among the moved bodies; `AGENTENGINE_WITH_HTTPS` is the
  only non-platform macro in `include/`, so no consumer sees a different layout.
- **Step 2.** The pump against each old worker, comments dropped: only the differences §4 lists; the order of the
  terminal checks, the producer/request/key lifetimes and the stop token are unchanged, and the positional arguments
  to both exchange functions match their signatures. `exchange_json` matches the old tail statement for statement.
  The fence code is byte-identical; the key is passed by `const&` and nothing new logs or copies it.
- **Build and link.** The three sources compile clean under g++-14 `-O2 -Werror`; their objects need only 3
  AgentEngine symbols from outside themselves, all in `agentengine_provider_http_client`. With HTTPS off, the new
  header, both client headers and `session_builder.hpp` compile standalone. All 50 executables whose include
  closure reaches either client link the library (control: 292 of 344 `json_value.hpp` includers do not). The
  layering lint flags an L3 include planted in the new sources (control) and passes on the real tree.
- **MINOR — weak self-control (fixed).** The only harness control differed in `code` on every pair, so a comparator
  ignoring `klass` or `message` would have passed it. Two controls that differ in one field each were added (§5).
- **MINOR — unfinished evidence line (fixed).** §5 "Builds" held a placeholder for the Linux run when it was read;
  the run finished and the line is filled in.
- **NOTEs.** The new transport cases do not reach `exchange_json`'s transport-error branch or check the request
  bytes; `test_chat_client_cross_backend_parity` drives `chat()` over real TLS on both backends (it passes, §5), and
  the transport-error path is `perform_provider_https_exchange`'s own, unchanged. The differential stops at status
  999 (nothing branches above 500). L2 client code now shares a static library with the L1 provider HTTP client:
  the lint checks includes, not libraries; the choice is §2's design B. `exchange_json` is public, but it adds no
  authority: `perform_provider_https_exchange` already is (I2).
