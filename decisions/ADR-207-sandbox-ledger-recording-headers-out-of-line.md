# ADR-207 — The Docker surface, the process-identity check, chat recordings and `Ledger<Store>`: bodies compiled once in `src/`

- **Status**: **Judged (2026-09-27, project-owner sign-off).** Re-checked on `main` before sign-off: controls C1, C3 and
  C4 re-run and caught (see "Re-check before sign-off" at the end). Original status: Proposed — design + implementation
  + proof (2026-09-27); red team round 1 (same day): no BLOCKER or MAJOR, 2 MINOR, both fixed (§8).
- **Date**: 2026-09-27
- **Origin**: GitHub issue #120 (S7), the structure audit after issue #115 closed.
- **Touches**: `include/agentengine/sandbox/docker_execution_surface.hpp` + `src/sandbox/docker_execution_surface.cpp`
  (new), `include/agentengine/sandbox/detail/process_identity.hpp` + `src/sandbox/process_identity.cpp` (new),
  `include/agentengine/core/chat_recording.hpp` + `src/core/chat_recording.cpp` (new),
  `include/agentengine/core/ledger.hpp` + `include/agentengine/core/ledger_impl.hpp` (new) + `src/core/ledger.cpp`
  (new), `CMakeLists.txt`, `tools/layers.toml`, three tests (§5).

## 1. The question

S7 named four more headers whose bodies every includer compiled again: `sandbox/docker_execution_surface.hpp`
(1,712 lines), `sandbox/mandatory_sandbox_provider.hpp`, `core/chat_recording.hpp` and `core/ledger.hpp`. #120 rated
it low payoff, since each has 14–25 including files (counted transitively), against 49–206 for S1, S5 and S6.

Two of them do more than cost compile time. The Docker header included `<windows.h>` (through
`sandbox/detail/process_identity.hpp` as well as directly) and, on POSIX, `<unistd.h>`, `<spawn.h>`, `<sys/wait.h>` and
`<poll.h>`, so every file that used the Docker surface got `<windows.h>`'s macros as a side effect (on Linux,
`<unistd.h>` still arrives through libstdc++'s `<atomic>`; the process headers do not). And
`Ledger<Store>` is a class template whose 25 large members were instantiated, and at `/O2` optimized, in every file
that used a ledger, though the codebase only ever instantiates it with two stores.

Can the bodies be compiled once without changing behaviour, and what does that buy?

## 2. Designs considered

- **A. Move the non-template bodies to `src/` verbatim, and give `Ledger<Store>` an implementation header plus
  explicit instantiations for its two stores (chosen).**
- **B. `extern template class Ledger<…>;` with the bodies left in the class.** A member defined inside the class is
  implicitly `inline`, and an explicit instantiation declaration does not suppress the instantiation of inline
  functions ([temp.explicit]/11). Compilers still instantiate them wherever they are used, so the saving does not
  materialize. The bodies have to leave the class for the declaration to mean anything, and once they have left,
  keeping them out of the header is what stops the instantiation. No `extern template` is then needed.
- **C. Hard-wire the two instantiations in `ledger.cpp` with the bodies there too.** `ledger.hpp` promises that
  `Ledger<Store>` stays generic over any `WorktreeObjectStore` conformer "without touching this file". With the
  bodies in a `.cpp`, a third store would have to edit that `.cpp`. `core/ledger_impl.hpp` keeps the promise: a new
  store includes it in one of its own files and instantiates there.
- **D. Explicit instantiations of `MandatorySandboxProvider<Surface, Store>`.** Its `Surface` parameter is an open set:
  Docker, containerd and several test surfaces (`PlainSurface`, fakes). There is no fixed list to instantiate, and
  the header has no non-template function bodies (the tool structs' `invoke()` bodies are one-line sentinels). It is
  left as is. It gains indirectly: each of its includers stops compiling `Ledger`'s bodies (§6).
- **E. Leave it.** Keeps `<windows.h>` in every Docker-surface includer and the ledger bodies in every ledger user.

## 3. Decision

Design A.

**Verbatim move.** The ADR-200/206 scripts, extended in two ways:
- the free-function script now records the `#ifdef _WIN32` / `#else` branch each function sits in and reproduces it
  around the definition in the `.cpp`;
- the class script counts braces in the first branch of an `#if` only, because `seed_tree_as_root()` opens the same
  block in both branches.

A third variant handles a class template: `template <class Store>`, `Ledger<Store>::` and
`typename Ledger<Store>::` on nested return types.

What moved (functions whose body is 6 or more lines; `process_identity.hpp`'s at any length, see below):

| Header | Moved | Header lines | Bodies now in |
|---|---|---|---|
| `sandbox/docker_execution_surface.hpp` | 14 free functions (Windows and POSIX process runners, argv rejection, isolation flags), 9 `DockerCliBackend` and 3 `DockerExecutionSurface` members | 1,712 → 755 | `src/sandbox/docker_execution_surface.cpp` |
| `sandbox/detail/process_identity.hpp` | all 10 functions | 211 → 106 | `src/sandbox/process_identity.cpp` |
| `core/chat_recording.hpp` | 16 functions | 429 → 185 | `src/core/chat_recording.cpp` |
| `core/ledger.hpp` | `merge_trees()` and 2 helpers; 25 `Ledger<Store>` members | 1,425 → 683 | `src/core/ledger.cpp`; `core/ledger_impl.hpp` |

The headers keep every declaration with its comment and default arguments, every type, the `constexpr` constants,
the inline variables (`g_next_container_seq`, `kOrphanNamePrefix`), constructors with initializer lists, member
templates, and the small helpers.

**The OS headers leave the public headers.** With the process runners out of line, nothing left in
`docker_execution_surface.hpp` names an OS API. ADR-203 §3 had kept `process_identity.hpp` inline because "both public
headers use [the OS headers] for their own process spawning" anyway; that premise is gone for Docker, so its 10
functions move too, at any length. The `#ifdef _WIN32` include blocks and `extern char** environ` move to the two
`.cpp` files. `containerd_execution_surface.hpp` (POSIX only) still includes its own POSIX headers, unchanged.

**Ledger.** `src/core/ledger.cpp` includes `ledger_impl.hpp` and `file_worktree_object_store.hpp` and instantiates
`template class Ledger<InMemoryWorktreeObjectStore>;` and `template class Ledger<FileWorktreeObjectStore>;`, the only
two specializations the codebase names (searched: `Ledger<`, `SandboxRuntime<`, `MandatorySandboxProvider<`,
`RealIoFileSystem<`, `BranchHandle<` across `include/`, `src/`, `tests/`, `tools/`, `examples/`, `bench/`). A file that
includes only `ledger.hpp` sees declarations, so it cannot instantiate the bodies and links to these instead. That is
well-formed: a function template specialization used in a file that does not define it must be explicitly
instantiated somewhere ([temp.pre]/10), and it is.

**Link.**
- The Docker and process-identity sources form `agentengine_docker_surface`, linked through `agentengine::core` like
  the ADR-199–201 libraries, so no consumer's CMake changes.
- `chat_recording.cpp` is `agentengine_chat_recording`, also linked through `agentengine::core`, with
  `agentengine_message_json` (the codec it wraps) as a `PUBLIC` dependency. It is not a second source of that
  library, because that one holds the V-layer codec and this file is L2 (red team, §8).
- `ledger.cpp` joins `agentengine_worktree_store` on both platforms. Every ledger user already links that library,
  because `InMemoryWorktreeObjectStore` calls its `compute_digest()`, so no consumer changes and no link cycle is
  added.

## 4. Behaviour — what is and is not the same

The claim is that no observable behaviour changes. What differs:

1. **Linkage.** The moved functions are no longer `inline`, so a caller in another file cannot inline them. None is
   on a hot path a caller inlined across files: each spawns a process, takes the ledger mutex, or builds JSON.
2. **Preprocessor branches.** Every moved function inside `#ifdef _WIN32` / `#else` is wrapped in the same branch in
   the `.cpp` (the Windows and POSIX `run_capture`, `run_argv`, `path_to_utf8`, `process_is_alive`,
   `read_process_start_ticks`, `current_process_start_key`, `process_start_key_for`). `_WIN32` is
   compiler-defined, so the library and every includer see the same value. `NOMINMAX` and `WIN32_LEAN_AND_MEAN`
   are directory-wide `add_compile_definitions` in the root `CMakeLists.txt`, so every target in this build sees
   them too; and since the headers no longer include `<windows.h>`, what an includer defines no longer reaches the
   moved bodies at all. `#if` blocks inside a body moved with the body.
3. **Statics.** One moved body has a function-local static: `Ledger<Store>::would_accept_blob_write`'s
   `static std::set<std::uint64_t> const kNoRoots`. Before, there was one per specialization, merged across files;
   now there is one per specialization in `ledger.cpp`. The same objects, and the set is const and empty. No moved
   body uses `thread_local`, `assert`, `__FILE__` or `source_location`.
4. **Instantiation.** An explicit instantiation instantiates every member, including ones no file used before with
   `FileWorktreeObjectStore`. They compiled cleanly under both compilers (§5); no member's behaviour depends on
   being instantiated.
5. **Includes.** A file that used `<windows.h>` or a POSIX header without including it, relying on the Docker header,
   no longer compiles. One did: `test_docker_run_argv_timeout.cpp` included `<tlhelp32.h>` before `<windows.h>`; it
   now includes `<windows.h>` first. Every file with a build target that includes the moved headers transitively
   (§5) builds on MSVC and g++. `bench/docker_image_digest_resolution.cpp` has no target (bench/README.md). It is
   built by hand, and its documented command lines now name the two `src/sandbox/` files; the g++ line links.
6. **Security.** The bodies moved verbatim, unchanged:
   - the Docker argv rejections (`docker_cli_reject_*`, ADR-146), the isolation flags (ADR-171) and the root-owned
     seed archive (ADR-174);
   - the fail-closed process-identity check (ADR-108/203);
   - the ledger's identity ACL (`authorized_for`, `insert_acl_root_bounded`, `mark_digest_shared`,
     `grant_parent_owner_access_locked`, ADR-102).
   What decides authority is the same code, now compiled in one place. §5a shows that the existing tests exercise
   the compiled-once copies, not a stale inline one.

## 5. Evidence

- **Moved verbatim.** A normalized line diff drops comments, blank lines and whitespace. It compares each old header
  (`main`) with the new header plus the files its bodies went to:

  | Header | Lines only in the old file | Lines only in the new files |
  |---|---|---|
  | Docker | 27 | 97 |
  | process identity | 10 | 42 |
  | chat recording | 16 | 41 |
  | Ledger | 9 | 115 |

  Every one of those lines is a signature (the old definition line, the new declaration, the definition without
  `inline` / default arguments / `[[nodiscard]]` / `static`), a qualified member name, a `template <class Store>`
  head, an `#include`, `#ifdef` or namespace line, or one of the two explicit instantiations. No body line differs.
- **The ledger bodies left the includers.** `dumpbin /symbols` on `test_ledger.cpp.obj`:
  - `main`: 50 `Ledger<…>` member functions defined in the object, `commit`, `merge`, `get_blob_safe` and
    `load_durable_state` among them.
  - S7: 5 defined (the inline constructor and small accessors), 14 referenced only, the four above among them.
  - `ledger.cpp.obj` defines 61, both specializations.
- **Builds.** MSVC 14.51, `dev` preset (`/O2` libraries, `/Od` tests, `/W4 /WX`): the four libraries plus every
  target whose source includes one of the four headers or `process_identity.hpp` transitively. That is 55 source
  files, found by an include-graph walk over `include/`, `src/`, `tests/`, `tools/`, `examples/` and `bench/`. No
  warnings. Linux: g++-14 Release (`-O2 -DNDEBUG`, `-Werror`),
  `AGENTENGINE_WITH_HTTPS=ON` (the CI leg's configuration), the same set: 44 targets, no warnings.
- **Tests.** MSVC, with a live Docker daemon (29.7.2): all 36 test targets among them pass. Linux (WSL, Docker
  29.1.3): all 37 pass. That includes the 11
  real-daemon Docker tests (`test_docker_isolation`, `test_docker_orphan_reap`, `test_docker_seed_ownership`,
  `test_docker_run_argv_timeout`, `test_docker_non_ascii_path`, `test_execution_surface_image_identity`,
  `test_sandbox_runtime`, `test_mandatory_sandbox_provider`, `test_task_branch_tools`,
  `test_task_branch_concurrent_dispatch`, `test_composed_sandbox_providers_live`), which run the moved process
  runners against real containers. It also includes the ledger, recording and replay tests. `python
  tools/layering_lint.py`: OK.
- **New tests.**
  - `test_docker_header_no_os_headers` does not compile if `docker_execution_surface.hpp` pulls in `<windows.h>`,
    `<spawn.h>`, `<sys/wait.h>` or `<poll.h>` again, so the benefit cannot silently regress. It does not check
    `<unistd.h>`: libstdc++'s own `<atomic>` includes it (`bits/atomic_wait.h`), and the header needs `<atomic>` for
    `g_next_container_seq`. A first version checked it, and the Linux build caught that.
  - Two new checks in `test_ledger`: a digest nobody wrote is a policy denial for `get_blob_safe` and
    `get_tree_safe`, even for the owner. Before them, no test pinned that branch (C1 below).

### 5a. Positive controls

Each control breaks one line in a body's new, out-of-line home, rebuilds, and runs the tests that cover it:

| Control | Caught by |
|---|---|
| C1: `authorized_for` treats a digest with no ACL entry as readable | `test_ledger` (the new unknown-digest checks). **Before those checks existed, no test caught it**: the store miss that followed still failed the read, only with class `fatal` and code `ledger.get_blob_failed` instead of `policy` / `ledger.blob_access_denied`, and no test looked at which. |
| C1b: `authorized_for` passes any non-zero identity through the owner/ancestor test | `test_ledger` |
| C2: `docker_cli_reject_embedded_nul` skips its NUL test | `test_sandbox_runtime` (real daemon) |
| C3: `check_process_identity` reports a dead pid as the same live process | `test_process_identity_equivalence`, `test_docker_orphan_reap` (real daemon) |
| C4: `error_to_json` records `"x"` instead of the error code | `test_chat_recording_codec` |
| C5: `test_docker_header_no_os_headers` against `main`'s two headers | fails to compile. MSVC: `#error "… pulls in <windows.h> again"`. g++-14: `#error "… pulls in <spawn.h> again"` and `"… <sys/wait.h> again"` |

C1–C4 edit only the new `.cpp` / `_impl.hpp` copies, so a test that catches them runs those copies. That settles
whether callers link to the compiled-once code or to something inlined from before.

## 6. Measurements

Single-file compile of an including file, MSVC 14.51, run serially, the faster of two runs. "Before" compiles the
same file against `main`'s four headers, "after" against S7's. CPU is `cl.exe`'s total (all threads).

`/O2 /Ob2` (what the Release and ASan CI legs compile tests with):

| File | before wall / CPU | after wall / CPU | CPU |
|---|---|---|---|
| `tests/core/ledger/test_ledger.cpp` | 3.2 / 8.4 s | 2.1 / 3.8 s | −54 % |
| `tests/sandbox/test_sandbox_runtime.cpp` | 4.4 / 11.2 s | 3.4 / 6.5 s | −42 % |
| `tests/sandbox/test_mandatory_sandbox_provider.cpp` | 7.6 / 24.8 s | 6.4 / 19.2 s | −23 % |
| `tests/sandbox/execution_surface/test_docker_isolation.cpp` | 3.0 / 5.0 s | 2.2 / 2.8 s | −45 % |
| `tests/core/chat/test_replay_chat_client.cpp` | 3.7 / 8.8 s | 3.1 / 6.4 s | −28 % |
| `tests/sandbox/test_task_branch_tools.cpp` | 8.0 / 25.1 s | 6.7 / 19.4 s | −23 % |

`dev` flags (`/Od`): −26 %, −15 %, −11 %, −10 %, +1 %, −10 % CPU for the same six files. The unoptimized build spends
less of its time in the bodies, so there is less to save. The new sources cost 10.3 s (`ledger.cpp`, both
specializations), 5.1 s (Docker), 5.6 s (chat recording) and 0.6 s (process identity) of CPU at `/O2`, once per
build. That is recovered after one or two of the 14–25 including files.

## 7. Residuals

- **`mandatory_sandbox_provider.hpp` is unchanged** (§2 D). Its own template bodies are still compiled in each of
  its 14 includers.
- **A third `WorktreeObjectStore`** used with `Ledger` needs its own explicit instantiation (`ledger_impl.hpp`'s top
  comment). Forgetting it is a link error naming the missing member, not a silent behaviour change.
- **No daemon-free test pins the Docker NUL rejection.** C2 is caught only by a test that needs a real daemon
  (`test_sandbox_runtime`). Pre-existing.
- **Full-tree builds.** Locally, only the targets that include a changed header were built (§5). The four CI legs
  build everything.

## 8. Red team

Round 1 (2026-09-27), an independent reviewer. It ran its own Linux build of the whole tree: g++-14 Debug,
`-Werror`, HTTPS off, 574 steps. No errors, warnings or undefined references; the 20 daemon-free affected tests pass.

- **MINOR — fixed.** `bench/docker_image_digest_resolution.cpp` (no CMake target, built by hand per its top comment)
  no longer linked with its documented command: the Docker bodies are in `src/` now. Both command lines name the two
  sources. §4.5 no longer claims more than the files with a target.
- **MINOR — fixed.** `chat_recording.cpp` had joined `agentengine_message_json`, the V-layer codec's library, as an
  L2 file. It is its own library now (§3).
- **NIT — fixed.** §4.2 called `NOMINMAX` / `WIN32_LEAN_AND_MEAN` "global"; they are directory-wide compile
  definitions.
- **NIT — left.**
  - The headers keep standard includes that only the moved bodies used (`<fstream>`, `<sstream>`, `<random>`,
    `<limits>`), because consumers may rely on them.
  - `ledger.cpp` puts both specializations in one object, so without `--gc-sections` a program that uses only the
    in-memory store also carries the file store's code: size, not behaviour.
  - C2 needs a daemon (§7).

Checked and found correct by the reviewer:
- every moved body matches its old text;
- each Windows/POSIX runner sits in its old branch;
- declaration qualifiers are unchanged;
- the inline variables and the single function-local static are handled correctly;
- no link-order hazard (`nm`);
- every ledger user links `agentengine_worktree_store`;
- the only `Store` arguments anywhere are the two instantiated ones;
- no `<windows.h>` macro name appears in the headers' identifiers;
- the MSVC ASan section counts are far below the `/bigobj` limit;
- the hygiene test's guard macros exist on both platforms;
- the positive controls' logic;
- the line counts.

## Re-check before sign-off (2026-09-27)

On `main` at f37912a: full `dev` build with `/W4 /WX`, no warnings; `ctest -LE live-network` with Docker 29.7.2 running,
363/363 pass (the two `*_no_process_creation` probes skipped as always, `test_external_skill_discovery` excluded).

The five headers are unchanged since the merge (598b8c1). `test_ledger.cpp.obj` still defines 5 `Ledger<>` members and
references 14, as in §5. The 18 `DockerCliBackend` symbols `test_sandbox_runtime.cpp.obj` defines are all the nested
`Instance` struct and its containers, none of the moved members. `test_chat_recording_codec.cpp.obj` defines no moved
recording function and references 4.

Controls re-run:

- C1, no ACL entry means readable: `test_ledger`'s unknown-digest checks fail.
- C3, a dead pid is the same process: `test_process_identity_equivalence` fails, and so does `test_docker_orphan_reap`
  against the real daemon ("the confirmed-dead-pid container WAS reaped").
- C4, `error_to_json` records `"x"`: `test_chat_recording_codec` fails (G1-R4, G1-R6).

Reverted; the tests pass.
