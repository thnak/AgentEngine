// GitHub issue #145: when `run_command`'s command RUNS but the commit after it fails, the model must still
// learn that the command ran, with its exit code and output.
//
// `SandboxRuntime::run()` executes the command (step 4) and only then clears, drains, scans and commits
// (steps 5-7). A failure in 5-7 -- `StorageBytes` exhausted is the realistic one, since that quota never
// refills and each commit charges the whole tree -- used to reach the model as that failure ALONE. A model
// that ran `git push` or `curl -X POST` then saw "storage quota exhausted" and concluded nothing happened.
//
//   R1 -- runtime: a commit failure after the command ran is still an ERROR (fail-closed, unchanged), and
//         `executed` holds the command's exit code and output.
//   R2 -- runtime: a command the surface refused never ran, so `executed` stays empty.
//   P1 -- provider: `run_command`'s tool error keeps its original code and class, and its message now says
//         the command DID run, with the exit code and the output.
//   P2 -- provider: a refused command's error says nothing about having run.
//   P3 -- provider: with storage to spare, the same command is an ordinary successful reply.
//
// Positive control: with `with_executed_output()` reduced to returning its input (a planted mutant, run by
// hand and recorded in the fixing commit), P1 fails.
//
// Needs no daemon: `PrintingSurface` is an in-test ExecutionSurface.

#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace agentengine;
namespace fs = std::filesystem;

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
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

constexpr char const* kMarker = "MARKER-145: the command really ran";

// Every command exits 3 and prints kMarker, except "refuse", which the surface rejects before running it --
// the "never attempted" shape `ExecutionSurface::run()` documents. `drain_to()` always leaves a 4 KiB file,
// so the commit has something to charge `StorageBytes` for.
struct PrintingSurface {
    [[nodiscard]] result<void> reset(fs::path const&) { return {}; }
    [[nodiscard]] result<SurfaceRunOutcome> run(std::string const& command) {
        if (command == "refuse") {
            return std::unexpected(error{failure_class::policy, "the surface refused the command", "test.refused"});
        }
        return SurfaceRunOutcome{3, kMarker};
    }
    [[nodiscard]] result<void> drain_to(fs::path const& host_dir) {
        std::error_code ec;
        fs::create_directories(host_dir, ec);
        std::ofstream(host_dir / "out.bin", std::ios::binary) << std::string(4096, 'x');
        return {};
    }
};
static_assert(ExecutionSurface<PrintingSurface>);

[[nodiscard]] fs::path fresh(std::string const& name) {
    fs::path const p = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

[[nodiscard]] bool contains(std::string const& hay, std::string const& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal const principal = make_embedded_principal("run-command-commit-failure-owner");
    IdentityHandle owner = authority.adopt(principal);

    auto run_quota = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    auto branch_quota = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    auto no_storage = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 16);
    auto storage = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    if (!run_quota || !branch_quota || !no_storage || !storage) {
        std::printf("[FAIL] quota setup\n");
        return EXIT_FAILURE;
    }

    // ---- R1/R2: SandboxRuntime::run() directly.
    {
        Ledger<> ledger;
        auto root = drive(ledger.create_root_branch(owner, "runtime"));
        if (!root) {
            std::printf("[FAIL] root branch\n");
            return EXIT_FAILURE;
        }
        SandboxRuntime runtime(ledger, std::move(*root), fresh("ae_test_145_runtime"));
        PrintingSurface surface;

        std::optional<SurfaceRunOutcome> executed;
        auto r = drive(runtime.run(surface, "echo", owner, *run_quota, *no_storage, &executed));
        check(!r.has_value(), "R1: a commit failure after the command ran is still an ERROR (fail-closed)");
        check(executed.has_value() && executed->exit_code == 3 && executed->stdout_text == kMarker,
              "R1: ... and `executed` holds the command's exit code (3) and output");

        std::optional<SurfaceRunOutcome> refused;
        auto rr = drive(runtime.run(surface, "refuse", owner, *run_quota, *storage, &refused));
        check(!rr.has_value() && !refused.has_value(),
              "R2: a command the surface refused never ran, so `executed` stays empty");
    }

    // ---- P1-P3: through run_command's own tool closure.
    auto invoke = [&](agentengine::rt::AsyncQuota<StorageBytes>& storage_quota, std::string const& label,
                      std::string const& command) {
        Ledger<> ledger;
        auto root = drive(ledger.create_root_branch(owner, label));
        MandatorySandboxProvider<PrintingSurface> provider;
        provider.bind_sandbox(ledger, std::move(*root), owner, fresh("ae_test_145_" + label), *branch_quota,
                              *run_quota, storage_quota);
        std::vector<Message> history;
        SessionContext session_ctx{label, principal, history};
        EffectContext ctx;
        ctx.principal = principal;
        CapabilitySet const caps = CapabilitySet::grant_root({});
        ctx.capabilities = borrow_capabilities(caps);
        auto contribution = drive(provider.on_context(session_ctx, ctx));
        result<json::Value> out =
            std::unexpected(error{failure_class::contract, "run_command not contributed", "test.none"});
        if (contribution) {
            for (auto const& tool : contribution->tools) {
                if (tool.name != "run_command") continue;
                out = tool.invoke(json::Value::make_object({{"command", json::Value::make_string(command)}}), ctx);
            }
        }
        return out;
    };

    {
        auto r = invoke(*no_storage, "p1", "echo");
        check(!r.has_value(), "P1: run_command with StorageBytes exhausted is a tool ERROR");
        if (!r.has_value()) {
            check(r.error().code != "test.none" && !r.error().code.empty(),
                  "P1: ... keeping the commit failure's own code (" + r.error().code + ")");
            check(contains(r.error().message, "DID run"), "P1: ... and its message says the command DID run");
            check(contains(r.error().message, "exit_code=3"), "P1: ... with the exit code");
            check(contains(r.error().message, kMarker), "P1: ... and the command's output");
        }
    }
    {
        auto r = invoke(*storage, "p2", "refuse");
        check(!r.has_value() && r.error().code == "test.refused" && !contains(r.error().message, "DID run"),
              "P2: a refused command's error is unchanged and says nothing about having run");
    }
    {
        auto r = invoke(*storage, "p3", "echo");
        check(r.has_value() && contains(json::dump(*r), "\"ok\":true") && contains(json::dump(*r), kMarker),
              "P3: with storage to spare, the same command is an ordinary successful reply");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
