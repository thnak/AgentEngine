# Research record — I/O reactor options for an all-async extension surface

**Compiled:** 2026-10-03 · **Status:** dated snapshot · **Feeds:** the planned "every extension point is
async" ADR (I/O reactor choice: sockets + TLS, DNS, child processes, timers, cancellation); touches
ADR-011 (resolve-once / connect-to-verified-literal), ADR-013 (mbedTLS TLS client), ADR-175 / ADR-219
(resume home, host `Resumer`), ADR-071 / ADR-209 (native process execution, Job Objects)

Triggered by the question: *AgentEngine does all network and child-process I/O with blocking calls today.
If every extension point becomes async, what should sit underneath `rt::task<T>` as the reactor — and
how much of it should we write ourselves?*

All version/date facts below were checked on 2026-10-03 against the cited primary source (official docs,
release pages, or the GitHub API). Library ecosystems move; re-check before relying on a version number.

---

## 0. What the repo already has (the constraints any choice must respect)

- **TLS is mbedTLS 3.6.7, vendored, exact-pinned and checksummed** (`CMakeLists.txt`, `ae_vendored_mbedtls`,
  behind `AGENTENGINE_WITH_HTTPS`, default OFF). ADR-013 (Judged) chose it and **explicitly rejected
  platform-native TLS (Schannel / OpenSSL)** "per explicit user direction, trading a bundled CA file's
  maintenance cost for one portable code path". A compiled-in, pinned CA bundle (curl.se dated snapshot
  2026-08-13) is embedded via `cmake/ca_bundle_embed.cpp.in`. → **Any reactor whose TLS story is
  "OpenSSL only" does not get to bring its TLS layer along**; the reactor must carry bytes for our
  mbedTLS session instead.
- **HTTP is in-house**: `src/sandbox/net_egress_proxy.cpp` (HTTP/1.1 exchange, streaming variant),
  `src/sandbox/tls_client.cpp` (`TlsClientSession::handshake` over an already-connected fd, blocking
  `send`/`recv`), `src/sandbox/provider_http_client.cpp`, `src/protocol/sse_stream_pump.hpp`. There is no
  libcurl, Beast, or cpp-httplib dependency.
- **DNS is resolve-once**: `resolve_and_validate` → `VerifiedEndpoint`, then connect to the verified
  literal and verify the certificate against the *original* hostname (ADR-011 anti-rebinding). Any HTTP
  layer that does its own resolution must be made to connect to our verified literal instead.
- **Process spawning is hand-written per platform**: `CreateProcess` + Job Objects on Windows
  (`src/backends/native_process/native_process_spawn.cpp`, `native_shell_session.cpp`,
  `src/backends/native_jail/job_object_limits.cpp`, which already creates an IOCP for Job notifications),
  `posix_spawn` + `poll()` pipe draining on Linux (`src/backends/kata/kata_backend_detail.hpp`).
- **Coroutine model**: `agentengine::rt::task<T>` (lazy, symmetric transfer) plus the ADR-175 / ADR-219
  "resume home" rule: a parked coroutine is resumed by the driver it belongs to (`block_on` thread, or a
  host-supplied `Resumer`), **never inline on the waker's thread**. The `resume_home.hpp` header already
  names "an IOCP thread" as exactly the kind of foreign waker this rule exists to keep out.
- **No inbound sockets**: host owns listeners (CLAUDE.md locked decision). The reactor only needs
  *client* TCP.

---

## 1. Asio (standalone) and Boost.Asio

### 1a. Status, version, license

- Standalone Asio **1.38.2** is the latest tag (`asio-1-38-2`; last upstream commit 2026-07-18) and is the
  same code shipped as Boost.Asio in **Boost 1.92.0**, released 2026-08-12.
  [GitHub tags](https://github.com/chriskohlhoff/asio/tags) ·
  [think-async.com](https://think-async.com/Asio/) ("Boost.Asio 1.38.2 is also included in Boost 1.92.0") ·
  [Boost releases](https://github.com/boostorg/boost/releases/tag/boost-1.92.0) ·
  [Boost 1.92.0 notes](https://www.boost.org/releases/1.92.0/)
- License: **Boost Software License 1.0** (permissive, MIT-compatible). [think-async.com](https://think-async.com/Asio/)
- Header-only by default; `ASIO_SEPARATE_COMPILATION` builds the implementation once into one TU.
  Requires C++11 minimum since 1.29.0. Optional deps: OpenSSL (only for `asio::ssl`), Boost.Coroutine
  (only for stackful `spawn`).
  [Using Asio](https://think-async.com/Asio/asio-1.38.2/doc/asio/using.html) ·
  [Revision history](https://think-async.com/Asio/asio-1.38.2/doc/asio/history.html)
- Recent releases (from the revision history): 1.31 made `deferred` the default completion token and
  added `cancel_after` / `cancel_at` timeout adapters; 1.33 added `asio::config` runtime tuning; 1.37 added
  `inline_executor`; **1.38.1 added optional binary versioning via inline namespaces "to allow multiple
  Asio versions in the same process"** (relevant if a host also links Asio); 1.38.2 added io_uring ring
  tuning parameters and `asio/fwd.hpp`.
  [Revision history](https://think-async.com/Asio/asio-1.38.2/doc/asio/history.html)

### 1b. Backends — Windows vs Linux parity

- **Windows**: overlapped I/O + **one I/O completion port per `io_context`** for all socket operations
  except async connect (emulated with `select` on a helper thread).
  [Implementation notes](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/implementation.html)
- **Linux**: `epoll` by default. With `ASIO_HAS_IO_URING`, io_uring is used for *file* operations; with
  `ASIO_HAS_IO_URING` + `ASIO_DISABLE_EPOLL` it replaces epoll entirely (Linux 5.10+).
  [Implementation notes](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/implementation.html)
- **DNS**: async resolution is *emulated* by "one or more additional threads per `io_context`" (default
  one) running blocking `getaddrinfo`. [Implementation notes](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/implementation.html)
  An in-flight `getaddrinfo` cannot be aborted, only abandoned.
- **Pipes**: `readable_pipe` / `writable_pipe` / `connect_pipe` — "portable anonymous pipes on POSIX and
  Windows (when I/O completion ports are available)". On Windows `connect_pipe` builds them from
  `CreateNamedPipeW` (overlapped), which is required because anonymous pipes from `CreatePipe` are
  synchronous-only (`CreatePipe` docs describe only blocking `ReadFile`/`WriteFile` semantics).
  [Pipes overview](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/pipes.html) ·
  [connect_pipe.ipp](https://github.com/chriskohlhoff/asio/blob/master/asio/include/asio/impl/connect_pipe.ipp) ·
  [CreatePipe](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-createpipe)
- **Process handles**: `asio::windows::object_handle::async_wait` waits on any kernel object, process
  handles explicitly included (implemented with `RegisterWaitForSingleObject`). On Linux a `pidfd` can be
  wrapped in `posix::stream_descriptor` and waited for readability.
  [object_handle overview](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/windows/object_handle.html) ·
  [win_object_handle_service.ipp](https://github.com/chriskohlhoff/asio/blob/master/asio/include/asio/detail/impl/win_object_handle_service.ipp)

### 1c. Coroutine integration with a *custom* task type

- Asio's async model is token-based: the final argument of every `async_*` call is a **completion token**,
  and the `async_result<Token, Signature>` trait turns it into a handler. `use_awaitable` (Asio's own
  `awaitable<T>`), `use_future`, `deferred`, and lambdas are all just tokens; "users create custom tokens
  by specializing `async_result`". [Completion tokens](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/model/completion_tokens.html)
- So integration with `rt::task<T>` does **not** require adopting `asio::awaitable`. The pattern is a
  ~150–300 line `use_rt_task` token whose `async_result::initiate` returns an awaiter: `await_suspend`
  records the resume home (ADR-175/219), launches the initiation with a handler that **posts** the
  coroutine handle to that home (never resumes it on the `io_context` thread), and `await_resume` returns
  `result<T>`. This keeps all Asio types out of our public headers.
- Alternatively, ops can be obtained as `deferred` operations and wrapped in a generic awaiter — same idea.

### 1d. Cancellation model

- Per-operation cancellation via `cancellation_signal` (producer) / `cancellation_slot` (consumer); a
  handler associates a slot via `get_cancellation_slot()` or `bind_cancellation_slot`. Three strengths:
  **terminal** (only safe to close/destroy the object), **partial** (well-defined side effects reported),
  **total** (no observable side effects). One signal/slot pair serves at most one operation at a time.
  [Cancellation](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/core/cancellation.html)
- Bridging is mechanical: our awaiter registers a `std::stop_callback` on the session's `std::stop_token`
  that posts `signal.emit(cancellation_type::terminal)` onto the I/O object's executor (Asio objects are
  not thread-safe, so the emit must run there). `cancel_after` gives per-op timeouts.

### 1e. TLS

- `asio::ssl` is **OpenSSL-only** ("OpenSSL is required to make use of Asio's SSL support"). It cannot
  carry our ADR-013 mbedTLS session. [SSL overview](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/ssl.html)
- That is fine: mbedTLS supports non-blocking BIO callbacks — `mbedtls_ssl_set_bio` with an `f_recv`
  that returns `MBEDTLS_ERR_SSL_WANT_READ` when the transport would block; the context stays valid and
  the call is retried when data arrives. An async `TlsStream` over an `asio::ip::tcp::socket` is a
  ~300-line adapter (read ciphertext into a buffer → feed via BIO → retry `mbedtls_ssl_read/handshake`).
  [mbedTLS ssl.h reference](https://mbed-tls.readthedocs.io/projects/api/en/development/api/file/ssl_8h/)

### 1f. Child processes — Boost.Process v2 (and "asio::process")

- There is **no `asio::process` in standalone Asio 1.38.2**; process support lives in **Boost.Process v2**,
  which since Boost 1.88 is the default (v1 deprecated, docs removed). v2 is "fully asio based", uses
  `pidfd_open` on Linux (SIGCHLD fallback), native process `HANDLE`s on Windows, closes unused fds by
  default, and offers `terminate()` / `request_exit()` / `interrupt()` and `process_stdio` wiring to Asio
  pipes. [Boost.Process 1.92 docs](https://www.boost.org/doc/libs/1_92_0/libs/process/doc/html/index.html) ·
  [Boost 1.88 docs](https://www.boost.org/doc/libs/1_88_0/doc/html/process.html)
- It is actively maintained (commits 2026-09-07, 2026-07-23; on 2026-04-24 the dependency was narrowed
  from Boost.Asio to a new `Boost::asio_core` target). [boostorg/process commits](https://github.com/boostorg/process/commits/develop)
- A `BOOST_PROCESS_V2_STANDALONE` mode exists in `v2/detail/config.hpp` that targets standalone `::asio`
  and `std::filesystem` (namespace `process_v2`), but the shipped CMake target still links Boost.Algorithm,
  Fusion, Filesystem, Optional, Tokenizer, etc. — standalone use means vendoring the headers/sources by
  hand, outside the supported build. [config.hpp](https://github.com/boostorg/process/blob/develop/include/boost/process/v2/detail/config.hpp) ·
  [CMakeLists.txt](https://github.com/boostorg/process/blob/develop/CMakeLists.txt)
- **No Job Object support in v2** (Job Object code exists only under `v1/detail/windows/`). v2 has
  `on_setup`/`on_success` initializer hooks and `creation_flags`, so `CREATE_SUSPENDED` + assign-to-job
  is possible as an extension, but not built in. [v2/windows](https://github.com/boostorg/process/tree/develop/include/boost/process/v2/windows)

### 1g. Cost

Asio is notoriously heavy on compile time when included widely; mitigated by `ASIO_SEPARATE_COMPILATION`
plus confining Asio to a handful of `src/` TUs behind a pimpl (no Asio in `include/agentengine/`). Runtime
binary cost is modest (a few hundred KB for sockets/timers/resolver/pipes).

---

## 2. libuv

- **1.53.0**, released 2026-09-24; MIT; Tier 1 on Linux (≥3.10, glibc ≥2.17), macOS ≥11, Windows ≥10
  (VS 2017+). 1.53 switched Unix spawning to `posix_spawn`, added `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` in
  Windows `uv_spawn`, io_uring file-request cancellation, and `uv_write_t` cancellation.
  [v1.53.0 release](https://github.com/libuv/libuv/releases/tag/v1.53.0) ·
  [SUPPORTED_PLATFORMS.md](https://github.com/libuv/libuv/blob/v1.x/SUPPORTED_PLATFORMS.md)
- **Process**: `uv_spawn` with stdio pipe containers, `exit_cb`, `uv_process_kill` (pid-reuse safe).
  [Process docs](https://docs.libuv.org/en/v1.x/process.html)
- **Job Object behaviour is a red flag for an embedded library.** In `src/win/process.c`, the first
  non-detached `uv_spawn` lazily creates a **process-global** Job Object
  (`KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION | SILENT_BREAKAWAY_OK`) and **assigns the calling
  process itself** to it ("We could remove ourself afterwards, but there doesn't seem to be a reason
  to"); each child is assigned *after* `CreateProcessW` returns (not created suspended unless detached),
  and failures go through `uv_fatal_error` (abort). For a library embedded in someone else's host, that
  mutates the host's own job membership and adds a race window before a child is contained — at odds
  with our ADR-071/ADR-209 per-command Job Object containment.
  [src/win/process.c](https://github.com/libuv/libuv/blob/v1.x/src/win/process.c)
- **Integration with `rt::task`**: C callback API, the loop owns handles with explicit `uv_close`
  lifetimes; every op needs a hand-written awaiter. `uv_loop_t` is single-threaded, so cross-thread
  cancellation must go through `uv_async_send`. Cancellation is coarse (`uv_cancel` for work/fs/getaddrinfo
  requests; close the handle for streams). Readiness model for TLS (no TLS of its own) — mbedTLS adapter
  needed as with Asio. Integration effort is higher than Asio's token route, and gains nothing on Windows
  (libuv also uses IOCP).

---

## 3. Direct platform APIs (write our own thin reactor)

**Windows** — IOCP for sockets (`WSARecv`/`WSASend`/`ConnectEx` overlapped), timers via a timer heap
driving `GetQueuedCompletionStatusEx` timeouts, overlapped named pipes (anonymous pipes cannot be
overlapped — see §1b), and process containment through Job Objects:

- `PROC_THREAD_ATTRIBUTE_JOB_LIST` assigns the child to job(s) **at creation** (Windows 10 / Server 2016+),
  closing the create-then-assign race; `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` restricts inherited handles.
  [UpdateProcThreadAttribute](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)
- `JOBOBJECT_ASSOCIATE_COMPLETION_PORT` posts `JOB_OBJECT_MSG_EXIT_PROCESS` / `ACTIVE_PROCESS_ZERO` etc. to
  an IOCP — but Microsoft states these messages "are intended only as notifications and their delivery
  to the completion port is not guaranteed" (only `JobObjectNotificationLimitInformation` limits are
  guaranteed). So exit detection must still wait on the process handle.
  [JOBOBJECT_ASSOCIATE_COMPLETION_PORT](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_associate_completion_port)
- Async DNS: `GetAddrInfoExW` with `lpOverlapped` / completion routine and `GetAddrInfoExCancel`
  (Windows 8+, Unicode only). [GetAddrInfoExW](https://learn.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfoexw)

**Linux** — `epoll` + `timerfd` + `eventfd`, `pidfd_open` (5.3+; the pidfd becomes `EPOLLIN` when the
child is a zombie), `pidfd_send_signal`, `CLONE_PIDFD`, `waitid(P_PIDFD)`.
[pidfd_open(2)](https://man7.org/linux/man-pages/man2/pidfd_open.2.html)

**io_uring caution** — Google reported 60% of kCTF VRP submissions exploited io_uring and disabled it on
ChromeOS and production servers and restricted it on Android.
[oss-security, 2023-06-17](https://www.openwall.com/lists/oss-security/2023/06/17/2) ·
[Phoronix](https://www.phoronix.com/news/Google-Restricting-IO_uring). The current Docker
(`moby/profiles` `seccomp/default.json`) and containerd (`contrib/seccomp/seccomp_default.go`) default
seccomp allowlists contain **no io_uring syscalls** (checked 2026-10-03 via the GitHub API; containerd
[issue #9048](https://github.com/containerd/containerd/issues/9048) closed 2023-12-01). An engine that
runs inside our own Docker sandbox profiles must not *depend* on io_uring → **epoll is the Linux
baseline**, io_uring at most an opt-in.

**Cost of writing our own**: a correct dual-backend reactor (completion model on Windows, readiness on
Linux, unified behind one awaiter API) with timers, cross-thread wakeup, per-op cancellation that is
race-free against completion, connect timeouts, half-close, and shutdown/draining is, by experience of
the projects above, several thousand lines plus a long tail of platform bugs (Asio's own history is a
list of exactly these). It would also re-derive what Asio already gives on Windows (`ConnectEx`, overlapped
named pipes, `RegisterWaitForSingleObject` waits). The *process* layer is different: it is small
(~600–900 lines both platforms) and is where our specific requirements (Job Objects, ADR-209 grant
re-checks) live — that part we largely own already.

---

## 4. `std::execution` (P2300) and networking in C++26/29

- P2300R10 `std::execution` was adopted for C++26 at St. Louis (June 2024). C++26 also ships an async
  scope (P3149) and a coroutine `task<T>` (P3552). **Not in C++26: networking, file I/O, time-based
  schedulers, async parallel algorithms (P3300) — "queued for C++29".**
  [Stream HPC, 2026-06-11](https://streamhpc.com/2026-06-11/asynchronous-and-parallel-programming-in-c26/) ·
  [P2300R9](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r9.html) ·
  [cplusplus/papers #1054](https://github.com/cplusplus/papers/issues/1054)
- **Shipping status**: cppreference's C++26 support table shows **no** support in libstdc++, libc++, or
  MSVC STL for either `std::execution` (P2300R10 + follow-ups) or `std::execution::task` (P3552R3).
  [cppreference C++26 compiler support](https://en.cppreference.com/w/cpp/compiler_support/26)
- **stdexec** (NVIDIA reference implementation): Apache-2.0 with LLVM exception, C++20, GCC 12+ / Clang
  16+ / MSVC 14.43+, actively pushed (2026-10-02), latest tag `nvhpc-26.05`; README warns it "is
  experimental and tracks an evolving standard. APIs may change without notice." Its only I/O scheduler is
  a **Linux io_uring context** — nothing for Windows, no processes, no TLS.
  [NVIDIA/stdexec](https://github.com/NVIDIA/stdexec)
- **libunifex** (Meta): `linux/io_uring_context.hpp`, `linux/io_epoll_context.hpp`,
  `win32/low_latency_iocp_context.hpp`; last tag v0.4.0, last push 2026-05-31; license field
  NOASSERTION on GitHub. Pre-standard sender vocabulary that diverged from final P2300.
  [libunifex](https://github.com/facebookexperimental/libunifex)
- **Networking papers**: P2762 (sender/receiver interface for networking, building on the Networking TS)
  and P3185/P3482 (a TAPS-based, connection-by-name, secure-by-default design). SG4 "encouraged the
  author to continue work in P3482's direction" and will issue a forward-looking C++29 paper.
  [P2762R2](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2762r2.pdf) ·
  [P3185R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3185r0.html) ·
  [P3482R1](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3482r1.html) ·
  [cplusplus/papers #2151](https://github.com/cplusplus/papers/issues/2151)
- **Conclusion**: `std::execution` provides no I/O by itself; standard networking is C++29 at the
  earliest, with no process support proposed. It is a *vocabulary* to stay compatible with later, not a
  reactor to build on now. (Design note: keeping our awaiters sender-adaptable — a `result<T>` completion
  plus a stop-token — is cheap insurance.)

---

## 5. Other candidates (one line each)

| Library | Status (checked 2026-10-03) | Why not the reactor |
|---|---|---|
| [libcoro](https://github.com/jbaldwin/libcoro) | Apache-2.0, v0.16.0 (2026-03-04) | Networking/TLS features: "MSVC not currently supported"; TLS is OpenSSL; brings its own `task<T>`. |
| [cppcoro](https://github.com/lewissbaker/cppcoro) | MIT, last commit 2022-09-02, last push 2024-01 | Unmaintained; README: Linux `io_service`/file I/O "not yet implemented". |
| [Seastar](https://seastar.io/) | Apache-2.0, active | "can run on Linux or OSv" — no Windows; shared-nothing framework that wants to own the process. |
| [folly](https://github.com/facebook/folly) coro | Apache-2.0, very active | Windows supported but "many tests are disabled"; huge dependency (glog, gflags, fmt, libevent, double-conversion…); own `Task`. |
| [concurrencpp](https://github.com/David-Haim/concurrencpp) | MIT, last push 2025-05 | Executors/timers only, no socket or process I/O. |
| [libdispatch](https://github.com/swiftlang/swift-corelibs-libdispatch) | Apache-2.0, active | C blocks/queues, no coroutine model, no process spawning; Windows port exists mainly for Swift. |
| [c-ares](https://github.com/c-ares/c-ares) | MIT, v1.34.8 (2026-07-07) | Not a reactor — but the standard cancellable async-DNS library with socket-callback integration, if we ever outgrow thread-pool `getaddrinfo`. |

---

## 6. HTTPS clients that could sit on a reactor

- **Boost.Beast** (BSL-1.0, active): HTTP/1 + WebSocket *vocabulary*, not a client — "This library is not
  a client or server" — and "Beast only works with Boost, not stand-alone Asio"; TLS through
  `asio::ssl::stream` (OpenSSL). Would drag in Boost and OpenSSL, contradicting ADR-013.
  [Beast introduction](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/introduction.html)
- **libcurl multi-socket** (curl license, MIT-style; 8.22.0 released 2026-09-02): `curl_multi_socket_action`
  + `CURLMOPT_SOCKETFUNCTION` / `CURLMOPT_TIMERFUNCTION` lets an external event loop drive it — a
  **readiness** model (on Windows it would need Asio's `null_buffers`/`async_wait` style readiness, not
  IOCP completions). Supports mbedTLS as a TLS backend (ALPN/HTTP/2 yes, no OS trust store), an in-memory
  CA bundle via `CURLOPT_CAINFO_BLOB` (mbedTLS since 7.81.0), and `CURLOPT_OPENSOCKETFUNCTION` which "gets
  the resolved peer address … and is allowed to modify the address or refuse to connect completely" —
  enough to enforce ADR-011's connect-to-verified-literal.
  [curl_multi_socket_action](https://curl.se/libcurl/c/curl_multi_socket_action.html) ·
  [TLS backends compared](https://curl.se/docs/ssl-compared.html) ·
  [CURLOPT_CAINFO_BLOB](https://curl.se/libcurl/c/CURLOPT_CAINFO_BLOB.html) ·
  [CURLOPT_OPENSOCKETFUNCTION](https://curl.se/libcurl/c/CURLOPT_OPENSOCKETFUNCTION.html) ·
  [license](https://curl.se/docs/copyright.html) · [releases](https://github.com/curl/curl/releases)
  Cost: a large new attack surface and dependency for features (HTTP/2, proxies, auth schemes, cookies)
  we do not need for provider calls and SSE.
- **cpp-httplib** (MIT, very active): blocking, thread-per-request by design — does not solve the problem.
  [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib)
- **Keep our own HTTP/1.1 + SSE**: the request writer, byte-capped response reader, no-redirect posture
  and SSE pump already exist and are tested; converting them from blocking `send`/`recv` to awaiting an
  async stream is a mechanical change. HTTP/2 is not required by any provider we target.

---

## 7. Comparison

| Option | Maturity / license | Win + Linux parity | Custom-`task` integration | Processes | Cancellation | Build / binary cost |
|---|---|---|---|---|---|---|
| **Asio 1.38.2 (standalone)** | 20+ yrs, BSL-1.0, releases 2026-07 | IOCP / epoll (io_uring optional); same API | Custom completion token, ~150–300 LoC, no Asio types in public headers | Primitives only (pipes, `object_handle`, `stream_descriptor` for pidfd) | Per-op slots, terminal/partial/total; `cancel_after` | Heavy headers → separate compilation + pimpl; small binary |
| Boost.Process v2 | Active, BSL-1.0 | Both; pidfd on Linux | Via Asio tokens | Yes, but **no Job Objects** | `terminate`/`request_exit`/`interrupt` | Pulls Boost unless hand-vendored in standalone mode |
| libuv 1.53.0 | Very mature, MIT | Tier 1 both (IOCP / epoll+io_uring) | C callbacks, hand-written awaiter per op, handle-close lifetimes | Yes, but global Job Object that enrolls the host process | Coarse (`uv_cancel`, close) | C library, moderate |
| Own reactor | n/a | We write both | Native | Native (we own Job/pidfd) | Whatever we build | Smallest deps, largest effort/risk |
| `std::execution` / stdexec | C++26 vocab; not in any STL; stdexec experimental | stdexec I/O Linux-only | Sender adaptors | None | stop-token native | n/a for I/O |
| libcurl multi | Very mature, curl lic. | Both (readiness) | Through a reactor | n/a | `curl_multi_remove_handle` | Large dep |

---

## 8. Recommendation

1. **Reactor: standalone Asio 1.38.x, vendored like mbedTLS** (FetchContent, exact URL + SHA-256, BSL-1.0),
   compiled once via `ASIO_SEPARATE_COMPILATION`, used only inside `src/` behind an engine-owned
   `rt::IoContext` façade. Run the `io_context` on an engine-owned reactor thread (or a host-supplied one);
   **completions never resume engine coroutines on that thread** — the `use_rt_task` completion token
   posts the parked handle to its ADR-175 home / ADR-219 host `Resumer`. Linux backend: **epoll**
   (do not define `ASIO_HAS_IO_URING` by default). Windows: IOCP. Enable Asio's inline-namespace
   versioning (1.38.1+) so a host linking its own Asio does not collide.
   *Why*: it is the only candidate with mature, symmetric Windows-IOCP + Linux support, a documented
   extension point (`async_result`) that lets us keep `rt::task<T>` as the only coroutine type, and a
   per-op cancellation model that maps onto `std::stop_token`.
2. **TLS: keep mbedTLS (ADR-013)** — write an async `TlsStream` that drives `mbedtls_ssl_*` via non-blocking
   BIO callbacks over an `asio::ip::tcp::socket`. Do **not** use `asio::ssl` (OpenSSL) or Beast.
3. **HTTP/SSE: keep our own** HTTP/1.1 + SSE code and convert it to await the async stream. Keep libcurl
   multi as a documented fallback if HTTP/2 or proxy support ever becomes a requirement.
4. **DNS**: Asio's resolver (thread-backed `getaddrinfo`) behind ADR-011's `resolve_and_validate`, which is
   already resolve-once; treat cancellation as "abandon, don't wait". Revisit `GetAddrInfoExW` / c-ares
   only if resolver-thread starvation shows up.
5. **Processes: own a thin layer on Asio primitives**, borrowing Boost.Process v2's design rather than
   depending on it. Windows: `CreateProcessW` + `PROC_THREAD_ATTRIBUTE_JOB_LIST` (contained at creation)
   + `HANDLE_LIST` + overlapped named pipes (`asio::connect_pipe`) + `windows::object_handle` for exit;
   kill = `TerminateJobObject`. Linux: `posix_spawn`/`clone3(CLONE_PIDFD)` + pipes as
   `posix::stream_descriptor` + pidfd readability for exit + `pidfd_send_signal` for kill. This is where
   ADR-071/ADR-209's existing Job Object and containment code already lives.
6. **Timers / cancellation**: `asio::steady_timer`, `cancel_after`; one `stop_token`→`cancellation_signal`
   bridge used by every awaiter.

Rejected: libuv (host-process Job Object enrolment and `uv_fatal_error` aborts in an embedded library;
callback API gives no integration advantage), a from-scratch reactor (re-derives Asio's Windows
machinery for no gain; spend the effort on the process layer instead), `std::execution`/stdexec/libunifex
(no portable I/O; not shipped), libcoro/cppcoro/Seastar/folly/concurrencpp/libdispatch (§5).

---

## 9. Risks

1. **Resume-home violation** — the easiest bug: a handler resuming a coroutine directly on the
   `io_context` thread. Needs a positive-control test (a coroutine parked on a socket read must resume on
   its `block_on` thread / via the host `Resumer`, asserted by thread id) and a test that fails if the
   token resumes inline.
2. **Cancellation races** — cancel vs. completion on different threads; Asio objects are not thread-safe,
   so emits must be posted to the object's executor. Use `terminal` semantics and close the object; test
   cancel-during-handshake, cancel-during-SSE-read, cancel-during-process-wait.
3. **Compile time / header leakage** — Asio in a public header would tax every TU; enforce "no Asio in
   `include/`" with the existing include-lint machinery (`cmake/check_testing_includes.cmake` precedent).
4. **API churn** — Asio changed the default token to `deferred` in 1.31 and keeps adding
   disposition/config machinery; pin an exact version and avoid `experimental::`.
5. **Windows async connect** uses a `select` helper thread; **async DNS** uses a helper thread — both
   are threads AgentEngine did not have before; account for them in shutdown and in "host owns
   threads" embeddings.
6. **mbedTLS async adapter correctness** — renegotiation off, `WANT_WRITE` during reads, close_notify
   handling; reuse ADR-013's certificate test corpus (untrusted root / hostname mismatch / expired) against
   the async path.
7. **Job Object semantics** — job IOCP notifications are not guaranteed (§3), so exit must be observed on
   the process handle; nested jobs need Windows 8+; `JOB_LIST` needs Windows 10 / Server 2016+.
8. **io_uring** — if ever enabled, it fails inside default Docker/containerd seccomp and has a poor
   security record; keep it an explicit opt-in, never the default.
9. **Host Asio collision** — a host embedding AgentEngine may link a different Asio/Boost.Asio; rely on
   1.38.1's inline-namespace versioning and test a two-version link.
10. **Future standard** — C++29 networking (TAPS-based, sender-shaped) may land; keep the façade small
    (`connect`, `read_some`, `write`, `wait_exit`, `sleep`) so the backend can be swapped.
