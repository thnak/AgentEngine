# ADR-174 — ADR-171's `--cap-drop ALL` made every host-seeded workspace file unreachable to the container's own root. Where does the ownership have to be decided?

- **Status**: Proposed — implemented, red-teamed in **two rounds** against two different
  implementations. Round 1 found four confirmed defects and the design was replaced rather than
  patched; round 2 found a **critical local arbitrary-file-overwrite vulnerability** in the
  replacement, plus eight lesser findings. All fixed and re-proven. Proven against live Docker
  daemons on both Windows and Linux, with causation established by reverting the fix and
  reproducing the original three CI failures on the same machine. Pending project-owner sign-off.
- **Date**: 2026-09-11
- **Closes**: GitHub issue #68
- **Narrows one consequence of ADR-171. Reopens none of its decisions.**
- **Files**: `include/agentengine/sandbox/ustar_writer.hpp` (new),
  `include/agentengine/sandbox/docker_execution_surface.hpp`,
  `tests/test_ustar_writer.cpp` (new), `tests/test_docker_seed_ownership.cpp` (new),
  `tests/CMakeLists.txt`, `.github/workflows/ci.yml`.

---

## 1. The question

ADR-171 closed a real gap: `DockerCliBackend::create()` emitted a `docker run` with no isolation
flags at all. Its fail-closed `ContainerIsolation` default includes `--cap-drop ALL`.

That is correct, and it broke the surface's other job.

`docker cp <host path> <container>:<path>` preserves the **host** file's ownership. The engine
materializes a worktree through `RealIoFileSystem::write_verified()`, which opens with
`open_within_mount_root(..., O_CREAT | O_TRUNC | O_WRONLY, 0600)` — mode **0600**, owned by whatever
user the host process runs as. `--cap-drop ALL` removes CAP_DAC_OVERRIDE and CAP_DAC_READ_SEARCH
from the container's root, and root without those is bound by the mode bits like anyone else. A
0600 file owned by uid 1001 is therefore neither readable nor writable by the container's uid 0.

The observable result: a container could work on files it had created itself, and could not touch a
single file it had been handed. That is every turn after the first. `test_sandbox_runtime` reported
it on the Linux CI leg as three failing checks.

Two framings were available and only one of them is right. The tempting one is "the container needs
the capability back". The question this ADR asks is narrower and better: **the container's view of
who owns its workspace is being decided by an accident of the host's filesystem. Where should it be
decided instead?**

## 2. What this does — and the line it does not cross

`docker cp -` reads a tar archive from stdin rather than copying a host path. What the container
ends up owning is then whatever the **archive headers** say. Writing those headers is what moves the
ownership decision from an accident to a statement.

**(a) `ustar_writer.hpp` — a POSIX ustar/PAX archive of a host directory's contents, uid/gid 0.**
Deliberately not a general-purpose tar library: it writes the one dialect `docker cp -` consumes,
from one source shape. Symlinks, devices, FIFOs and sockets are REJECTED — not followed and not
silently skipped, because a symlink seeded into a container's workspace is a path-escape primitive
and a dropped entry would make the container's view differ from the ledger's without saying so.
Hardlinks are **not** rejected: a hardlinked regular file is a regular file by every check, so it is
archived as a second full copy, which is what `docker cp` did before. An earlier version of the
header comment claimed otherwise and was simply wrong (round-2 finding F10).

**It streams.** The archive is written to an `std::ostream` in bounded chunks as the tree is walked,
and the caller streams that to a temp file which becomes the child's stdin. Peak memory is one 64 KiB
chunk regardless of tree size. §3's red-team section explains why this is the second design and not
the first.

**Mode is 0600 for files and 0700 for directories — the same bits `write_verified()` already opens
with.** Widening to 0644/0755 would also have worked, since the container's only principal is root
and root now owns every entry. It was rejected deliberately: it would be a permission change
smuggled in under an ownership fix. The seed changes **who owns the tree and nothing else**.

**(b) `run_argv()` learns to take its stdin from a file, on both platforms.** A defaulted
`std::filesystem::path const* stdin_file = nullptr`; callers that pass nothing get byte-identical
behaviour including the existing `/dev/null` stdin on POSIX. On POSIX this is one extra
`posix_spawn_file_actions_addopen`, which runs in the child after fork, so there is no parent-side
descriptor to leak or close. On Windows it is one inheritable `CreateFileW` handle closed as soon as
the child has it.

**(c) `DockerCliBackend::seed_tree_as_root()` and the one call site.**
`DockerExecutionSurface::reset()` seeds through it instead of `copy_to_container()`.
`copy_to_container()` itself is **unchanged** — it is a faithful `docker cp` wrapper, a public method
with its own test, and its host-ownership behaviour is what makes this ADR's negative control
possible.

**A second, unplanned property:** `host_dir` never reaches a command line at all. `copy_to_container()`
has to embed a host path as an argv element and validate it. The tar path has no host-path argument.
(The first draft claimed this meant "that whole class of question does not arise"; §3 records why
that was half wrong.)

**The line not crossed:** this does not relax ADR-171. `--cap-drop ALL`, `--network none`,
`--security-opt no-new-privileges` and every ceiling stay exactly as they were, and the live test
asserts the fix works **with them in place**.

## 3. Red-team round — four confirmed defects in the first implementation

The first implementation passed 34 offline checks, 10 live checks on two platforms, and a
revert-and-reproduce causation test. An adversarial review then executed it against a live daemon
and found four confirmed defects, three of them regressions against the very behaviour it replaced.
All four are fixed; each has a named regression guard.

**H1 — it refused filenames `docker cp` had always accepted, and the sandbox could brick itself.**
Two separate causes. The `..` check was a **substring** search, so `config..bak`, `..hidden` and
`notes..txt` were refused. And a path too long for ustar's 100-byte name / 155-byte prefix split was
refused outright, which the draft justified by saying pax "buys only the long-name case this file
refuses outright" — circular, as the review pointed out, because the refused case is a case that
**worked before**: Docker's own Go tar writer auto-upgrades to PAX. Verified against a live daemon,
`docker cp` accepted a 101-character basename and a 337-character nested path that the draft
rejected.

The severity is not the refusal, it is **when** it lands. The model's own code inside the sandbox
creates these names — one `cp notes.txt notes..bak`, one `npm install`. The failure then surfaces a
turn LATER, on the next `reset()`, and the surface never seeds again for the rest of the session.
Fixed: the `..` check is per-component, and long names are written as PAX extended headers
(typeflag `x`), which is exactly what the reader on the other end already expects.

**H2 — on Windows, a non-ASCII filename either corrupted the container's tree or threw.** The walk
used `generic_string()`. The review executed `path(L"café.txt").generic_string()` on this host and
got CP1252 bytes, which land in the container as invalid UTF-8; and `path(L"東京.txt")` **throws
`std::system_error`** ("no mapping for the Unicode character in the target multi-byte code page"),
propagating out of a `result<>` function, which CONVENTIONS forbids. This is precisely the defect
`docker_cli_detail::path_to_utf8()` exists to prevent on the argv side — the draft's claim that the
tar path avoids "that whole class of question" was wrong: the question moved from the command line
into the tar header. Fixed with `generic_u8string()`, which is UTF-16 to UTF-8 on Windows and cannot
fail for a name the filesystem holds.

**H3 — the I8 cap did not bound memory, and the failure mode was `std::terminate`.** The payload was
fully materialized twice before the cap was consulted. Measured by the review: one 1 GiB file spent
**2.0 GiB of RSS** on its way to being refused for exceeding a 256 MiB cap, and under a memory
ceiling it aborted with an uncaught `bad_alloc` rather than returning an error. `dd if=/dev/zero
of=big bs=1M count=4096` inside the sandbox, drained to the host, would have OOM-killed the host
engine on the next `reset()`. The cap had also quietly become a hard ceiling on worktree size that
`docker cp` never had.

Fixed by the streaming redesign rather than by moving the check. Re-measured after:

| Case | First implementation | Now |
| --- | --- | --- |
| 1 GiB file, success path | 516 MB peak RSS on a 250 MB file | 4.4 MB |
| 1 GiB file, refusal path | 2.0 GiB peak RSS | 4.4 MB |
| 1 GiB file under a 400 MB address-space ceiling | `std::terminate`, SIGABRT | succeeds, exit 0 |

The remaining cap is a **disk** guard checked from `file_size()` before any read, raised to 2 GiB.

**H4 — the new live test was not on the CI exclusion lists.** It carries `RESOURCE_LOCK
docker_daemon` and would have gone red on all four daemon-less Windows legs. This is the third time
this hand-maintained list has been missed, and the workflow's own comment already says so. Caught by
the review before it reached CI, which is the only reason it is not a third red run. A lint over the
`RESOURCE_LOCK docker_daemon` property would end the class.

**Three medium findings, also fixed.** `std::filesystem::relative()` is `weakly_canonical`-based and
**follows symlinks**, so an out-of-tree symlink was reported as a bogus path traversal
(`unsafe_path: ../../etc/passwd`) instead of as a symlink — an operator would have hunted for an
escape that did not exist; now `lexically_relative()`, with the type check moved ahead of the path
check. A TOCTOU window let a regular file be swapped for a symlink between the walk and the read, so
the target's bytes would be archived under the original name; the type is now re-checked at open
time. And a POSIX descriptor lacked `FD_CLOEXEC` — moot now, since the file-stdin redesign has no
parent-side descriptor at all.

**The redesign removed more than it fixed.** The first implementation pushed the archive through a
pipe from a dedicated writer thread, which needed SIGPIPE masking, a join ordered after the kill, and
an in-memory copy of the whole archive. The review confirmed all of that was *correct* — it traced
every early-return path and found no leak, deadlock or double-close. It was still four mechanisms
solving a problem the OS solves with one `addopen`. Staging to a temp file deleted the thread, the
pipe, the signal handling and the memory ceiling together.

**What the review cleared.** The tar format itself, three independent ways: a hexdumped header
against the POSIX field offsets, a strict Python `tarfile` `USTAR_FORMAT` parse, GNU `tar -tvf`, and
a live `docker cp -` round trip. No off-by-one in the name/prefix split. Empty source directories,
empty files and the `!exists(host_dir)` early return. No silent truncation under memory pressure. No
path escape through the archive. And the premise itself — the reviewer expected the daemon to force
ownership to root and verified that it does not.

## 3b. Red-team round 2 — the replacement had a worse bug than anything in round 1

Round 1's clearances did not transfer: the mechanism had changed from an in-memory archive pushed
through a pipe to a streamed archive staged in a temp file. Round 2 reviewed the replacement.

**F1 — CRITICAL, and demonstrated end to end: a local arbitrary-file-overwrite primitive.** The
staged archive went to `<temp>/ae_seed_<pid>_<g_next_container_seq>.tar`. On Linux `<temp>` is `/tmp`,
mode 1777. And `g_next_container_seq` is the **same counter that mints container names** — which
`docker ps` **publishes** as `ae_des_<pid>_<ticks>_<seq>`. So any local user could read one container
name, compute the next staged filename exactly, plant a symlink there, and have this code truncate
and overwrite any file the engine's uid can write.

The reviewer executed it. A victim file went from 24 bytes of text to 2048 bytes of tar; the cleanup
guard then deleted only the symlink, erasing the evidence; and `seed_tree_as_root()` returned
**success**. If the host process runs as a service account or root, that is local privilege
escalation. `docker ps` is not even required — an inotify watcher sees one name and knows every
subsequent one.

This is worth stating plainly: the fix for a containment regression introduced a vulnerability
strictly more serious than the bug it was fixing. It was found because the review was told to attack
the temp file specifically and asked whether the counter "interacts with the container-naming scheme
it was built for". It does, fatally, and that interaction is invisible unless you look for it.

Fixed two ways, both needed. The name is now unpredictable (`std::random_device`, 128 bits, not an
observable counter), and the archive lives inside a **private directory created 0700 in one atomic
`::mkdir`** — so even a guessed name lands somewhere no other user may create an entry.
`create_directory()` followed by `permissions()` was rejected: it is 0755 for the width of the window
between the two calls, which is all the attack needs. Verified by observing a real seed:

```
STAGE DIR: mode=700 owner=<engine user> name=/tmp/ae_seed_12256_4a108be9d90626868b1a8aa22eec25d5
  ARCHIVE: mode=600 owner=<engine user>
```

**F2 — the staged archive was mode 0644: the whole worktree readable by every local user.**
`std::ofstream` obeys the umask. The code comment and this ADR both claimed "0600-equivalent … not a
new exposure class"; the reviewer measured `mode=644` during a live seed, so both were simply false.
The private directory now denies traversal, and the file's own bits are stated explicitly rather than
inherited.

**F3 — the I8 cap undercounted by up to 11x.** The projection counted one header block plus the
padded payload, and nothing for the PAX header, its padded record, or the two terminating blocks. A
tree of long-named entries — exactly what an agent creates — overshot a cap set to its own projection
by a measured **5.6x**, and by arithmetic ~11x for a 4096-character path, so the 2 GiB cap admitted
roughly 22 GiB of staging. Compounding it: `/tmp` is **tmpfs** by default on systemd and WSL, so an
undercounted disk guard is a memory ceiling again — the very thing the streaming redesign existed to
remove. Now counted exactly: setting the cap one block below an archive's measured size refuses it,
and the projection matches the actual byte count to the block.

**F4 — an exception escaped a `result<>` function, and my first fix for it was worse.** The walk used
a range-for, which calls the **throwing** `operator++`; an unreadable subdirectory aborted the
process. Replacing it with `increment(ec)` and checking `ec` at the top of the loop body looked
correct and was not: **a failed increment leaves the iterator equal to end**, so the loop exits and
the check never runs again. Measured: the archive came back *successful* carrying a single entry,
having silently dropped everything the walk had not reached. Trading a crash for a silent truncation
is the worse of the two bugs — a container view that quietly differs from the ledger's is exactly
what this design refuses symlinks to prevent. The check now sits immediately after the increment, and
`U13` pins both failure modes with a positive control.

**F5 — a Windows handle without `FILE_SHARE_DELETE`** would have made the staged archive undeletable
while a timed-out `docker.exe` still held it, so the cleanup guard would fail *silently* and leave a
worktree copy in `%TEMP%` forever. **F7 — the round-1 UTF-8 fix had no test at all**, which
CONVENTIONS forbids for a load-bearing invariant; `U12` now asserts the actual UTF-8 bytes, stated
literally rather than derived from the conversion under test. **F8 — two branches in the live test
could record no check at all** and still print ALL PASS, the same degrade-to-silence pattern its own
skip messages argue against. **F9 — a directory that became a regular file between the two passes**
still shipped as a directory entry; the type is now re-checked for both branches before the header is
written. **F10 — documentation that did not match the code**: the header claimed hardlinks were
rejected when they are archived as a second copy, a Windows junction was reported as a "symlink,
device, FIFO or socket", and the new CMake block had been spliced between a neighbouring test's
comment and its target.

**What round 2 cleared, by execution.** The PAX implementation against GNU tar 1.35, Python
`tarfile`, and the live daemon, including the record-length fixed point straddling a digit boundary
and long *directory* names. Streaming correctness under a file that **grows** mid-copy (exactly
`e.size` bytes written, archive still synchronized) and one that shrinks (hard failure, no corrupt
archive shipped). Cap arithmetic overflow, not reachable. Temp-name collisions across threads and
processes. stdin-from-a-file with a missing or unreadable path on both platforms. And no round-1
clearance regressed.

## 4. Alternatives not chosen

- **`--cap-add DAC_OVERRIDE`.** A two-word fix that hands the container write access to every file
  on its filesystem, not just its own workspace. Rejected.
- **`--user <host uid>`.** Arguably stronger containment overall, and genuinely attractive. It is a
  much larger behavioural change — the image's own paths and `mkdir -p /workspace` have to tolerate
  a non-root user — and would have been an unrelated posture change riding inside a regression fix.
  Named as available future work.
- **Widen the mode `write_verified()` materializes at, to 0666.** Raised by the red-team, and it
  deserves the direct answer it asks for rather than silence: with 0666 the existing
  `copy_to_container()` path works unchanged under `--cap-drop ALL`, with no new file format and no
  archive at all. It is rejected because the 0600 is not incidental — it is the **host-side**
  protection on the worktree, and every other local user on the host is a principal there even
  though the container has only one. Trading a host-side confidentiality property for a
  container-side convenience is a worse trade than writing a tar header, and it would weaken a
  boundary this ADR is not otherwise touching. The review's argument that "mode carries no
  information here" holds inside the container and does not hold on the host.
- **Streaming straight into a pipe rather than via a temp file.** This is what the first
  implementation did; see §3.

## 5. Evidence

**Causation, not correlation.** Reverting `docker_execution_surface.hpp` alone via `git stash`, on
the same machine, daemon and build, reproduces the exact three original failures — turn 2's exit
code, turn 2 seeing turn 1's content, and the rollback's content. With the fix applied,
`test_sandbox_runtime` passes. Reduced further, with no AgentEngine code involved:

```sh
ISO="--network none --memory 536870912b --pids-limit 128 --cpus 1.000 \
     --cap-drop ALL --security-opt no-new-privileges"
H=$(mktemp -d); printf 'turn-1 content' > $H/note.txt
docker run -d --rm -w /workspace --name c $ISO alpine:latest sh -c "mkdir -p /workspace && sleep infinity"
docker cp "$H/." c:/workspace
docker exec c sh -c 'cat note.txt && echo -n " + turn-2 addition" >> note.txt'
#   with $ISO:     turn-1 contentsh: can't create note.txt: Permission denied   [exit=1]
#   without $ISO:  turn-1 content                                              [exit=0]
```

**`tests/test_ustar_writer.cpp` — 55 checks, offline.** Needs no daemon, network or privileges, so
it runs on every CI leg including the ones with no container runtime at all. U1-U6 assert the archive
bytes: uid/gid 0 and uname/gname `root`; the POSIX `ustar\0` + `00` magic specifically, not GNU tar's
older `ustar  \0`; the checksum a reader recomputes with the field read as eight spaces; block
padding and the two terminating zero blocks; directory typeflag, trailing slash and emission order;
and the 0600/0700 modes. U7 and U8 are negative controls with their own stable codes, U8 paired with
a positive control that the same tree under an adequate cap succeeds.

U9-U11 are the regression guards for H1: `config..bak` and `..hidden` are archived, a genuine `..`
component is still refused, and a long path goes through PAX while one with a legal prefix split
still uses the prefix field and emits no PAX header.

**`tests/test_docker_seed_ownership.cpp` — 17 checks, live.** S1 reads the seeded file's uid from
**inside the container** rather than inferring it from what we sent. S3 is issue #68 itself: the
container writes a file it did not create, with `--cap-drop ALL` in place, and a second check
confirms the write landed so a zero exit cannot be a false pass. S6 drives the real `reset()` →
`run()` path `SandboxRuntime` uses. S7 puts H1's three names through the real daemon, so the PAX
header is proven against **the daemon's own tar reader** and not only against our bytes.

**S5 is the negative control that makes S1 mean anything.** It asserts `copy_to_container()` still
delivers host ownership — without it, S1 could be passing because something about the environment
made every file root-owned. On a Docker Desktop/Windows host `docker cp` has no host uid to preserve
and hands everything to root anyway, so the control cannot discriminate there; the test **detects
that and reports it as skipped rather than passed**. Two other checks skip honestly for the same
reason: symlink creation needs elevation on Windows, and a 241-character nested path exceeds
MAX_PATH so the host file is never created.

| Platform | Result |
| --- | --- |
| Linux (WSL2 Ubuntu, g++-14, live Docker 29.7.2) | ustar 55/55, seed ownership 17/17 with every control running, `test_sandbox_runtime` all passed |
| Windows (MSVC 14.51, live Docker Desktop) | ustar 53/53, seed ownership 15/15 with honest skips, full suite 349/349 |

Both repo lints OK. Windows builds clean under `-Werror`.

## 6. Promotion gate

G1 — a container seeded by `seed_tree_as_root()` reports uid 0 for a seeded file, read from inside
the container, on a host where `docker cp` demonstrably does not (the S5 control runs, not skips).
**Met**, Linux.

G2 — the container writes a file it did not create, with `--cap-drop ALL` in place, and the write is
observed to have landed. **Met**, both platforms.

G3 — the failing `test_sandbox_runtime` checks pass with the fix and fail without it, on the same
machine and build. **Met.**

G4 — the archive's bytes are asserted by a test that needs no daemon, so this ADR keeps evidence on
every CI leg rather than only where a container runtime exists. **Met**, 55 checks.

G5 — every name plain `docker cp` accepted is still accepted. **Met**: U9/U11 offline and S7 against
the real daemon.

G6 — peak memory is independent of worktree size, and an oversized tree is refused rather than
aborting the process. **Met**: 4.4 MB peak on a 1 GiB file on every path, and success under a 400 MB
address-space ceiling where the first implementation called `std::terminate`.

G7 — the staged archive is unreachable to other local users, by name and by permission. **Met**:
observed on a live seed at directory mode 0700 with a 128-bit random name and file mode 0600, closing
a demonstrated arbitrary-file-overwrite primitive.

## 7. Residuals, named not hidden

- **The archive is staged to a temp file**, which transiently holds a second copy of the worktree's
  bytes. Not a new exposure class (the worktree is already on disk unencrypted), created under the
  process umask, and removed on every exit path by a guard — but it is disk the previous
  implementation did not use, and a genuinely streaming pipe would not need it. The pipe version was
  built first and traded away for the reasons in §3.
- **2 GiB archive cap.** `docker cp` had no equivalent limit. Deliberate and raisable, not a claim
  that more is unsafe.
- **PAX is emitted only for `path`.** A file large enough to overflow ustar's 12-byte size field
  (8 GiB) would need a PAX `size` record; it is refused with `ustar_writer.field_overflow` instead,
  which the 2 GiB cap makes unreachable in practice.
- **A single `mtime` for every entry**, captured once per archive. `materialize()` wrote the whole
  tree moments earlier, so this is as accurate as a per-file stat without a per-entry
  `file_clock`-to-`system_clock` conversion.
- **Backend contract divergence (round-2 finding F6), not resolved here.** Docker's `reset()` now
  refuses a tree containing a symlink or a Windows junction; `ContainerdExecutionSurface::reset()`
  seeds through a live bind mount and accepts both. CONVENTIONS says backends may differ in
  *strength* but not in *contract*. It is not reachable through `SandboxRuntime::run()` today —
  `RealIoFileSystem::materialize()` does `remove_all` and then writes only regular files — but
  `reset(host_dir)` is a public `ExecutionSurface` method and a host may call it with any directory.
  Left as a named residual rather than silently widened: relaxing the refusal would give back the
  path-escape primitive, and making containerd refuse too is a change to a surface this ADR has no
  evidence for.
- **`ContainerdExecutionSurface` is untouched.** It seeds through a live bind mount and never had
  this bug — confirmed by reading its test, not assumed.
