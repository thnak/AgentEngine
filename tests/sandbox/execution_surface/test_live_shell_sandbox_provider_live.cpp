// ADR-209 build step 8 (GitHub issue #146) -- `LiveShellSandboxProvider<DockerPersistentShellSurface>` end to end
// against a REAL Docker daemon (claims C1, C2, C3, C7, C12, C16 live). REQUIRES a running daemon and the
// `python:3.12-alpine` and `alpine:latest` images (pulled if absent); excluded from the Windows CI legs like every
// daemon test (ci.yml).
//
// Machine safety (CLAUDE.md): containers run under ADR-171's isolation with a 128-pid / 512 MiB ceiling, this
// process caps its own memory, and an in-process watchdog _Exit()s past 560 s (ctest's TIMEOUT is the outer one).
//
//   L0  C1's control: Tier 0 (`SandboxRuntime::run`) does NOT carry an `export` to the next command.
//   L1  C1: `python -m venv .venv && . .venv/bin/activate`, `cd`, `export` persist to the next shell_exec, the
//       venv's python runs there, and BOTH commits succeed -- the venv's links skipped and listed.
//   L2  C2: one container for a 3-turn, 5-command run.
//   L3  C3: a deletion is committed.
//   L4  C16: after a reset that drops `ghost.txt`, the next command re-opens a FRESH container where it is absent,
//       and it is never re-committed.
//   L5  C12 (a measurement): a background writer's output is committed by the next command, under the owner; how
//       much of it a mid-write drain captures is recorded, not asserted (§6: torn sets are disclosed).
//   L6  C7 through the provider: a timeout loses the shell, the next command re-opens with the last snapshot's cwd.
//   L7  PerRun: run end removes the container; the destructor removes a held one.
//
// Positive controls (planted by hand, recorded in ADR-209 §15): scanning with `is_regular_file` (following links,
// the #142 bug) fails L1's commit; forcing a re-open on every command fails L2; not clearing staging before the
// drain fails L3; reusing the container on re-open fails L4.

#include "agentengine/sandbox/docker_execution_surface.hpp"
#include "agentengine/sandbox/docker_persistent_shell_surface.hpp"
#include "agentengine/sandbox/live_shell_sandbox_provider.hpp"

#include "../../support/crt_fail_fast.hpp"
#include "../../support/memory_cap.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

using namespace agentengine;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;
void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
    std::fflush(stdout);
}

[[nodiscard]] std::string show(std::string s) {
    for (auto& c : s) {
        if (c == '\n') c = '|';
    }
    return s.size() > 300 ? s.substr(0, 300) + "..." : s;
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] bool container_exists(std::string const& id) {
    if (id.empty()) return false;
    return docker_cli_detail::run_argv({"docker", "inspect", "-f", "{{.Id}}", id}, 30).exit_code == 0;
}

[[nodiscard]] ContainerIsolation box() {
    ContainerIsolation iso;
    iso.pids = 128;
    iso.memory_bytes = 512ull * 1024 * 1024;
    return iso;
}

void ensure_image(std::string const& image) {
    if (docker_cli_detail::run_argv({"docker", "image", "inspect", image}, 30).exit_code != 0) {
        (void)docker_cli_detail::run_argv({"docker", "pull", image}, 300);
    }
}

using Provider = LiveShellSandboxProvider<DockerPersistentShellSurface>;

[[nodiscard]] EffectContext owner_ctx() {
    EffectContext ctx;
    ctx.principal.id = "live-owner";
    return ctx;
}

[[nodiscard]] std::optional<ToolDescriptor> tool_of(Provider& p) {
    static std::vector<Message> const none;
    EffectContext ctx = owner_ctx();
    SessionContext sc{"live", ctx.principal, none};
    auto contribution = drive(p.on_context(sc, ctx));
    if (!contribution.has_value()) return std::nullopt;
    for (auto& d : contribution->tools) {
        if (d.name == "shell_exec") return d;
    }
    return std::nullopt;
}

[[nodiscard]] result<ShellExecReply> shell(Provider& p, std::string const& command) {
    auto tool = tool_of(p);  // an on_context per call: each is a "turn" boundary as far as the provider can tell
    if (!tool.has_value()) return std::unexpected(error{failure_class::contract, "no tool", "test.no_tool"});
    EffectContext ctx = owner_ctx();
    auto out = tool->invoke(json::Value::make_object({{"command", json::Value::make_string(command)}}), ctx);
    if (!out.has_value()) return std::unexpected(out.error());
    return schema::from_json<ShellExecReply>(*out);
}

[[nodiscard]] bool tree_has(Ledger<>& ledger, std::string const& tree, IdentityHandle who, std::string const& name) {
    auto t = ledger.get_tree_safe(tree, who);
    return t.has_value() &&
           std::any_of(t->entries.begin(), t->entries.end(), [&](auto const& e) { return e.name == name; });
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    (void)agentengine::test_support::cap_process_memory(768ull << 20, 2048ull << 20);
    std::thread([] {
        std::this_thread::sleep_for(560s);
        std::printf("[FAIL] WATCHDOG: test exceeded its 560 s budget\n");
        std::fflush(stdout);
        std::_Exit(3);
    }).detach();
    ensure_image("alpine:latest");
    ensure_image("python:3.12-alpine");

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.adopt(Principal{.id = "live-owner", .tenant_id = ""});
    Ledger<> ledger;
    auto branch_q = *agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    auto run_q = *agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 1000);
    auto storage_q = *agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 4'000'000'000ull);
    auto open_q = *agentengine::rt::AsyncQuota<LiveShellOpen>::mint_root(authority, owner, 100);
    auto reset_q = *agentengine::rt::AsyncQuota<ResetCost>::mint_root(authority, owner, 100);
    fs::path const dir = fs::temp_directory_path() / "ae_test_lssp_live";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // ---- L0: Tier 0 does not persist an export.
    {
        auto root = drive(ledger.create_root_branch(owner, "tier0"));
        SandboxRuntime runtime(ledger, std::move(*root), dir / "tier0");
        DockerExecutionSurface surface("alpine:latest", box());
        (void)drive(runtime.run(surface, "export X=1", owner, run_q, storage_q));
        auto r = drive(runtime.run(surface, "echo \"[$X]\"", owner, run_q, storage_q));
        check(r.has_value() && r->exec.stdout_text.find("[]") != std::string::npos,
              "L0 (C1 control): Tier 0 does not carry an export to the next command");
    }

    auto bind = [&](Provider& p, std::string const& name, std::string const& image, std::chrono::milliseconds deadline) {
        auto root = drive(ledger.create_root_branch(owner, name));
        return p.bind_sandbox(ledger, std::move(*root), owner, dir / name, branch_q, run_q, storage_q, open_q,
                              LiveShellLimits{30min, 2h, 100},
                              [image] { return DockerPersistentShellSurface(image, box()); }, deadline);
    };

    // ---- L1, L2, L3, L4, L5
    {
        Provider p;
        check(bind(p, "main", "python:3.12-alpine", 120s).has_value(), "setup: bind the live provider");
        auto venv = shell(p, "mkdir -p sub && python -m venv .venv && . .venv/bin/activate && cd sub && export MARK=kept");
        check(venv.has_value() && venv->ok && !venv->skipped_symlinks.empty(),
              "L1: the venv command commits, its links skipped and listed (" +
                  (venv ? std::to_string(venv->skipped_symlinks.size()) + " links, " + venv->commit_error : venv.error().message) + ")");
        auto use = shell(p, "python -c 'import sys; print(sys.prefix)'; pwd; echo \"[$MARK]\"");
        check(use.has_value() && use->ok && use->output.find("/workspace/.venv") != std::string::npos &&
                  use->output.find("/workspace/sub") != std::string::npos && use->output.find("[kept]") != std::string::npos,
              "L1: the next shell_exec runs the venv's python, in the same cwd, with the export (" +
                  (use ? show(use->output) : use.error().message) + ")");
        std::string const container = p.surface() != nullptr ? p.surface()->container_id() : std::string{};

        (void)shell(p, "cd /workspace && echo one > f.txt");
        auto rm = shell(p, "rm f.txt");
        check(rm.has_value() && rm->ok && !tree_has(ledger, rm->tree_digest, owner, "f.txt"), "L3: a deletion is committed (C3)");
        check(p.opens() == 1 && p.surface()->container_id() == container,
              "L2: one container across a 5-command, multi-turn run (C2)");

        auto keep = shell(p, "echo k > keep.txt");
        (void)shell(p, "echo g > ghost.txt");
        auto reset = keep.has_value() ? drive(p.reset_to_turn(keep->turn_index, owner, reset_q))
                                      : agentengine::result<Checkpoint>(std::unexpected(keep.error()));
        auto after = shell(p, "ls ghost.txt 2>/dev/null || echo absent");
        check(reset.has_value() && after.has_value() && after->reopened && after->output == "absent\n" &&
                  !tree_has(ledger, after->tree_digest, owner, "ghost.txt") && p.surface()->container_id() != container &&
                  !container_exists(container),
              "L4: after a reset, a FRESH container where the dropped file is absent and never re-committed (C16)");

        auto bg = shell(p, "(i=0; while [ $i -lt 40 ]; do echo $i >> bg.log; i=$((i+1)); sleep 1; done) >/dev/null 2>&1 &");
        auto mid = shell(p, "sleep 1; wc -l < bg.log");
        auto head_mid = ledger.head_checkpoint(*p.branch_name(), owner);
        auto late = shell(p, "sleep 3; wc -l < bg.log");
        check(bg.has_value() && mid.has_value() && mid->ok && tree_has(ledger, mid->tree_digest, owner, "bg.log") &&
                  head_mid.has_value() && head_mid->authored_by_id == owner.id(),
              "L5: a background writer's output is committed by the next command, under the owner (C12)");
        std::printf("       L5 measurement: mid-write drain saw %s line(s), the later one %s (of 40)\n",
                    mid ? show(mid->output).c_str() : "?", late ? show(late->output).c_str() : "?");
    }

    // ---- L6, L7
    {
        std::string held;
        {
            Provider p;
            check(bind(p, "timeout", "alpine:latest", 3s).has_value(), "setup: bind with a 3 s deadline");
            // A file inside `deep`: the Ledger keeps regular files only, so an EMPTY directory would not exist in the
            // re-opened container and the replayed `cd` would (correctly) fall back to /workspace (ADR-209 §12 item 8).
            (void)shell(p, "mkdir -p deep && echo x > deep/f && cd deep");
            auto t = shell(p, "sleep 60");
            auto next = shell(p, "pwd");
            check(t.has_value() && t->timed_out && t->shell_lost && next.has_value() && next->reopened &&
                      next->output == "/workspace/deep\n",
                  "L6: a timeout loses the shell; the next command re-opens in the last snapshot's cwd (C7) [" +
                      (t ? "timed_out=" + std::to_string(t->timed_out) + " lost=" + std::to_string(t->shell_lost) + " out=" + show(t->output) : t.error().message) +
                      "] [" + (next ? "reopened=" + std::to_string(next->reopened) + " out=" + show(next->output) : next.error().message) + "]");
            std::string const id = p.surface()->container_id();
            EffectContext ctx = owner_ctx();
            (void)drive(p.on_run_end(RunEndView{run_end_reason::final_answer}, ctx));
            check(!container_exists(id) && !p.holds_environment(), "L7: PerRun run end removes the container");
            (void)shell(p, "true");
            held = p.surface()->container_id();
        }
        check(!held.empty() && !container_exists(held), "L7: the destructor removes a held container");
    }

    fs::remove_all(dir, ec);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
