// ADR-132's own §5 disclosed residual, closed here: "nothing in this ADR changes any real production
// caller to actually USE a non-default Store... a real host wanting durable content still has to
// explicitly instantiate `MandatorySandboxProvider<Surface, FileWorktreeObjectStore>` itself." This is
// that host -- the FIRST real, user-reachable production consumer of `agentengine::
// FileWorktreeObjectStore` (ADR-130), `bind_root_branch()` (ADR-128), and the `Store`-generic tool
// surface (ADR-132), composed together exactly the way `tests/test_task_branch_content_durability_
// integration.cpp` already proved works, but through a REAL interactive CLI a real user runs, not a test.
//
// Deliberately a SEPARATE, small tool rather than a change to tools/sandboxed_shell_chat.cpp itself --
// the identical reasoning that file's own header comment already gives for staying separate from
// tools/cli_chat.cpp applies again here: this tool's own DURABLE identity/ledger/store wiring is a real,
// distinct choice a host makes (persistent state across process restarts, on real disk, under the
// user's own home directory), not a drop-in replacement for the already-shipped, already-verified
// ephemeral-by-design tool. Zero risk to that already-shipped tool; this file changes nothing it depends
// on.
//
// THE SECURITY PRECONDITION THIS TOOL GETS RIGHT, DELIBERATELY, NOT BY ACCIDENT
// (decisions/ADR-130-content-durability-conformer.md's own `test_identity_durability_precondition.cpp`
// proved this the hard way): making `Ledger`'s own content durable WITHOUT ALSO durably configuring
// `IdentityAuthority::bootstrap()` turns a previously-latent id-recycling risk into a REAL, working
// cross-principal content leak on a process restart. This tool configures BOTH `Ledger`'s own
// `durable_dir` AND `IdentityAuthority::bootstrap()`'s own `durable_dir`, under the SAME real, persistent
// root, consistently -- never one without the other. A future host copying this file's own pattern
// should not silently drop either half.
//
// REQUIRES: Windows or Linux, a running Docker daemon reachable via the `docker` CLI on PATH, and
// OPENAI_API_KEY set in the environment -- the same real-world requirements
// tools/sandboxed_shell_chat.cpp already documents for the identical DockerExecutionSurface it also uses.

#include "agentengine/core/file_worktree_object_store.hpp"
#include "agentengine/core/session_builder.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/sandbox/docker_execution_surface.hpp"
#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace {

// `USERPROFILE` on Windows, `HOME` everywhere else -- the same real, researched OS convention
// `include/agentengine/core/external_skill_discovery.hpp`'s own `host_home_dir()` already uses;
// duplicated here (not shared) rather than reaching into that unrelated feature's own `_detail`
// namespace for a five-line helper. Uses `agentengine::pal::env_var()` (not raw `std::getenv()`) for the
// same portable, already-established reason every other real env-var read in this codebase does.
[[nodiscard]] std::optional<std::filesystem::path> host_home_dir() {
#if defined(_WIN32)
    if (auto v = agentengine::pal::env_var("USERPROFILE")) return std::filesystem::path(*v);
#else
    if (auto v = agentengine::pal::env_var("HOME")) return std::filesystem::path(*v);
#endif
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace agentengine;

    {
        DockerCliBackend orphan_sweep;
        auto swept = orphan_sweep.reap_orphans();
        if (swept.has_value() && (swept->reaped > 0 || !swept->reap_failures.empty())) {
            std::cerr << "startup orphan sweep: inspected " << swept->inspected << ", reaped "
                      << swept->reaped << ", " << swept->reap_failures.size()
                      << " destroy failure(s)\n";
        } else if (!swept.has_value()) {
            std::cerr << "startup orphan sweep skipped (non-fatal): " << swept.error().message << "\n";
        }
    }

    std::string const model = argc > 1 ? argv[1] : "gpt-4o-mini";
    std::string const session_id = "durable-sandboxed-shell-chat-session";

    // ---- REAL, PERSISTENT durable state, under the user's own home directory -- survives across --
    // ---- process restarts, which is the entire point of this tool. Falls back to a temp-directory ---
    // ---- location (still real, just not guaranteed to survive an OS temp-cleanup) if HOME/USERPROFILE
    // ---- is unavailable, disclosed here rather than silently: a host relying on genuine durability ---
    // ---- across a real reboot should not run this tool in an environment with no home directory. -----
    std::filesystem::path const durable_root =
        host_home_dir().value_or(std::filesystem::temp_directory_path()) / ".agentengine" /
        "durable_shell_chat";
    std::filesystem::path const ledger_durable_dir = durable_root / "ledger";
    std::filesystem::path const identity_durable_dir = durable_root / "identity";
    std::filesystem::path const objects_dir = durable_root / "objects";

    // ---- The identity-native sandbox, DURABLE identity AND DURABLE content, configured together, ----
    // ---- never one without the other (see this file's own top comment). --------------------------
    IdentityAuthority& authority = IdentityAuthority::bootstrap(identity_durable_dir);
    Principal const cli_principal{"cli-user", ""};
    IdentityHandle const owner = authority.adopt(cli_principal);

    auto branch_quota = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    auto run_quota = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 10'000);
    auto storage_quota =
        agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 100'000'000);
    auto merge_quota = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 1'000);
    if (!branch_quota.has_value() || !run_quota.has_value() || !storage_quota.has_value() ||
        !merge_quota.has_value()) {
        std::cerr << "FATAL: failed to mint this session's real sandbox quotas\n";
        return 1;
    }

    Ledger<FileWorktreeObjectStore> cli_ledger(FileWorktreeObjectStore(objects_dir), ledger_durable_dir);

    std::filesystem::path const staging_root =
        std::filesystem::temp_directory_path() / ("ae_durable_sandboxed_shell_chat_" + session_id);

    // ---- bind_root_branch() (ADR-128), through the Store-generic tool surface (ADR-132): resolves --
    // ---- (reclaim-if-orphaned, create-if-not) THIS owner's own deterministic root branch by identity --
    // ---- alone -- a genuine crash-recovery flow, exercised for real on every run of this tool, not ----
    // ---- merely in a test. A prior run that ended uncleanly (a crash, a killed process, Ctrl+C) -------
    // ---- leaves its root branch as a real orphan; this call reattaches to it automatically. ----------
    using SandboxProvider = MandatorySandboxProvider<DockerExecutionSurface, FileWorktreeObjectStore>;
    SandboxProvider sandbox_provider;
    auto bound = sandbox_provider.bind_root_branch(cli_ledger, owner, staging_root, *branch_quota,
                                                     *run_quota, *storage_quota, session_id);
    if (!bound.has_value()) {
        std::cerr << "FATAL: failed to bind this session's real, durable sandbox: "
                  << bound.error().message << " (" << bound.error().code << ")\n";
        return 1;
    }
    sandbox_provider.bind_task_branch_tools(*merge_quota);

    // ---- The session itself, via the already-shipped, already-tested §2b builder -------------------
    using Builder =
        quickstart::ComposedQuickstartSessionBuilder<quickstart::Provider::openai, InMemorySecretStore,
                                                        SandboxProvider>;
    auto built = Builder(model)
                     .session_id(session_id)
                     .principal(cli_principal)
                     .grant(Capability{cap::RunCommand{}})
                     .api_key_from_env("openai-api-key", "OPENAI_API_KEY")
                     .providers(std::make_tuple(std::move(sandbox_provider)))
                     .build();
    if (!built.has_value()) {
        std::cerr << "FATAL: failed to build the session: " << built.error().message << " ("
                  << built.error().code << ")\n";
        if (built.error().code == "quickstart_builder.no_store") {
            std::cerr << "Set OPENAI_API_KEY in the environment before running this tool.\n";
        }
        return 1;
    }

    std::cout << "durable_sandboxed_shell_chat -- run_command (Docker-backed, DURABLE identity-native "
                 "ledger + content), composed via the Store-generic tool surface into one real session.\n"
                 "Durable state: " << durable_root.string() << "\n"
                 "Every task branch you start survives a crash -- kill this process and restart it to "
                 "see bind_root_branch() reattach automatically.\n"
                 "Type a message, or 'exit' to quit.\n";

    std::string line;
    while (true) {
        std::cout << "\n> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line == "exit" || line == "quit") break;
        if (line.empty()) continue;

        auto reply = built->ask(line);
        if (!reply.has_value()) {
            std::cerr << "error: " << reply.error().message << " (" << reply.error().code << ")\n";
            continue;
        }
        std::cout << *reply << "\n";
    }

    std::cout << "Goodbye. Durable state preserved at " << durable_root.string() << " -- run this tool "
                 "again to resume.\n";
    return 0;
}
