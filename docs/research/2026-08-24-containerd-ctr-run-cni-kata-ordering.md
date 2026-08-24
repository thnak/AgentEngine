# `ctr run --cni` vs. Kata Containers: a real, source-verified lifecycle-ordering incompatibility

2026-08-24. Sources: `github.com/containerd/containerd` (`main`) and
`github.com/kata-containers/kata-containers` (`main`), both fetched via
`curl -sL raw.githubusercontent.com/...` into this session's scratchpad and read directly with
Grep/Read -- not summarized from memory or WebFetch, per this project's "external claims are dated
and cited" convention and this session's own established methodology (see
`docs/research/2026-08-24-containerd-ctr-run-config-vs-convenience-flags.md`, which used the same
approach for `pids`/`fds`).

## 1. The question

`KataBackend::create()` currently fails closed on any `NetPolicy` beyond `deny_all` (Slice 2/3,
unchanged) -- no network endpoint is ever created for the guest VM today. Before writing that off
as "not investigated", this note checks whether `ctr run`'s own `--cni` convenience flag
(`run.go:131-132`, confirmed to exist -- unlike `--pids-limit`, which does not) can drive a real,
CNI-plugin-enforced allowlist for a Kata-runtime container.

## 2. `ctr run --cni`'s actual sequence (containerd `run.go`)

Read directly from `run.go`'s `Action` closure:

1. `container, err := NewContainer(ctx, client, cliContext)` -- builds the OCI spec / container
   object (no netns population yet).
2. `task, err := tasks.NewTask(ctx, client, container, ...)` -- **this is containerd's task-CREATE
   step.**
3. Only **after** `NewTask` returns: `netNsPath, err := getNetNSPath(ctx, task)`, then
   `network.Setup(ctx, commands.FullID(ctx, container), netNsPath)` -- **this is where the CNI
   plugin actually runs and populates the netns with a veth.**
4. `task.Start(ctx)` -- containerd's task-START step, run last.

`getNetNSPath` (`run_unix.go:513`): `return fmt.Sprintf("/proc/%d/ns/net", task.Pid()), nil` -- the
netns CNI configures is whatever netns owns `task.Pid()` **as of right after CREATE**, before START.

This ordering (create empty netns at CREATE, populate via CNI between CREATE and START, exec the
real workload at START) is exactly matched to `runc`'s own create/start split: `runc create` forks
the container's init process into a **fresh, empty** network namespace and stops before exec;
`runc start` is what actually execs the entrypoint. CNI slotting in between is deliberate and safe
for `runc`.

## 3. Kata's actual sequence (virtcontainers `api.go`, `sandbox.go`, shimv2 `create.go`)

Read directly from `src/runtime/virtcontainers/api.go`, `CreateSandbox()`:

```go
func CreateSandbox(ctx context.Context, sandboxConfig SandboxConfig, factory Factory, prestartHookFunc func(context.Context) error) (VCSandbox, error) {
    ...
    if err = s.createNetwork(ctx); err != nil {   // line 82
    ...
    if err = s.startVM(ctx, prestartHookFunc); err != nil {   // line 92
```

`createNetwork()` (`sandbox.go:1156`) calls `s.network.AddEndpoints(ctx, s, nil, false)` -- this
**scans the sandbox's already-configured netns for existing veth/tap endpoints** and wires the
TC-filter redirection into the VM's tap device (`docs/design/architecture/networking.md`'s own
"TC-Filter" section, fetched and read directly: "a redirection is created between the container
network and the virtual machine" -- driven by whatever interfaces are present in the netns at the
moment this scan runs).

`containerd-shim-v2/create.go` calls `katautils.CreateSandbox(...)` (which wraps the function above)
**synchronously inside the shim's `Create()` task RPC handler** -- i.e. during containerd's
task-CREATE step, the exact step `ctr run`'s `tasks.NewTask()` triggers. `s.startVM()` (which boots
the actual hypervisor) also runs inside this same synchronous call, before `Create()` returns.

So for a Kata-runtime container, by the time `tasks.NewTask()` returns in `run.go` (step 2 above),
**both** the network-endpoint scan **and** the VM boot have already completed, using whatever the
netns looked like at CREATE time. `network.Setup()` in step 3 populates that netns only
**afterward** -- too late for Kata's own endpoint scan to see it. `task.Pid()` itself
(`GetHypervisorPid()`, confirmed in `containerd-shim-v2/create.go:205`) further underscores this is
the hypervisor's own netns, not a runc-style pre-exec container process netns.

## 4. The production-working order, confirmed by contrast (containerd CRI plugin)

`internal/cri/server/sandbox_run.go`'s `RunPodSandbox` handler -- the path Kubernetes/CRI actually
uses, and the one Kata+CNI is documented and widely deployed against -- does the **opposite** order
from `ctr run --cni`:

```go
// comment at sandbox_run.go:181-186, quoted verbatim:
// XXX: What we really want here is to call controller.Platform() and then check
// platform.OS, but that is only populated after controller.Create() and that needs to be
// done later (uses sandbox.NSPath that we will set just _after_ this).
```

and at line ~195-266: the netns is created and `c.setupPodNetwork(ctx, &sandbox)` (CNI ADD) runs
**before** `controller.Create()` (the sandbox/task-creation call) -- `sandbox.NetNSPath` is set and
baked into the generated OCI spec *before* Kata's `CreateSandbox()` ever scans it. This is exactly
the order Kata's synchronous, eager `createNetwork()`-then-`startVM()` sequence needs, and it is not
an accident: CRI's own comment (quoted above) shows this ordering was chosen deliberately.

## 5. Conclusion

`--cni` is a real, existing `ctr run` flag (unlike `--pids-limit`), but it drives an
orchestration order (CNI-after-CREATE) that is correct for `runc` and **incompatible with Kata's own
create-time network-endpoint discovery**, verified directly against both projects' real source, not
inferred from behavior reports or issue trackers. A Kata-runtime container launched via
`ctr run --cni` would have its netns populated by CNI only after Kata has already scanned it (empty)
and booted the VM -- no working network path would reach the guest.

The order that does work (CRI's) requires creating and populating the netns **before** the
container-create call -- i.e. before generating the OCI spec at all, which for `ctr` means
`--config` mode (a hand-built OCI spec with `linux.namespaces` already pointing at a pre-populated
netns), not the `--cni` convenience flag. See
`decisions/ADR-093-kata-backend-netpolicy-allowlist-investigated-and-deferred.md` for how this
folds into the same `--config`-exclusivity blocker `decisions/ADR-090-kata-backend-pids-limit-investigated-and-deferred.md`
already found for `pids`.
