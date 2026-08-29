// ADR-106 (decisions/ADR-106-containerd-execution-surface-promotion.md) residual, closed here:
// "Not wired into tools/cli_chat.cpp/tools/sandboxed_shell_chat.cpp or any real session builder --
// this ADR promotes the CONFORMER... A future pass wiring ContainerdExecutionSurface into a real,
// user-reachable tool surface is real, contained, disclosed follow-on work, not attempted here."
//
// A near-verbatim port of tools/sandboxed_shell_chat.cpp (ADR-102 Phase 5), swapping
// MandatorySandboxProvider<DockerExecutionSurface> for MandatorySandboxProvider<
// ContainerdExecutionSurface> -- see that file's own top comment for the full reasoning behind every
// structural decision reused here unchanged (a separate small tool rather than touching
// tools/cli_chat.cpp; reusing ComposedQuickstartSessionBuilder/Bundle::ask() rather than a hand-rolled
// driver; ordinary return rather than std::_Exit()). Composes the SAME two providers
// (SandboxToolProvider for `run_shell`, MandatorySandboxProvider<Surface> for `run_command`) into ONE
// session via ComposedContextProvider<Ms...> -- this file's only real difference from its sibling is
// the ONE template argument and the container image default (`docker.io/library/alpine:latest`,
// matching ContainerdExecutionSurface's own constructor default, since `ctr`'s own image-reference
// grammar expects a fully-qualified registry path unlike bare `docker` CLI shorthand).
//
// Linux-only (ContainerdExecutionSurface's own scope: `ctr` only meaningfully talks to a local
// containerd Unix socket) and, unlike its Windows sibling, has NO Linux-side blocker of its own left:
// SandboxToolProvider has been portable since ADR-103; QuarantineSecretStore/hmac_sha256 (the one real
// blocker that kept the Docker sibling Windows-only for a time) has been portable since ADR-107 --
// this tool needed neither fix, it simply never existed on either platform before this pass.
//
// REQUIRES: Linux, a running containerd daemon reachable via the `ctr` CLI on PATH (root or an
// unprivileged containerd-socket ACL -- containerd's default socket permissions require it, matching
// tests/test_containerd_execution_surface.cpp's own disclosed precondition), and OPENAI_API_KEY set
// in the environment.

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/session_builder.hpp"
#include "agentengine/sandbox/containerd_execution_surface.hpp"
#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"
#include "agentengine/trust/secret_quarantine.hpp"
#include "backends/native_jail/sandbox_tool_provider.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

int main(int argc, char** argv) {
    using namespace agentengine;

    std::string const model = argc > 1 ? argv[1] : "gpt-4o-mini";
    std::string const session_id = "containerd-shell-chat-session";

    // ---- The identity-native sandbox: ledger/authority/quotas/root branch (ADR-102 Phases 1-4) -----
    // See tools/sandboxed_shell_chat.cpp's own top comment for the lifetime-ordering requirement this
    // declaration order satisfies (BranchHandle::~BranchHandle() dereferences a raw Ledger<>* back to
    // cli_ledger).
    Ledger<> cli_ledger;
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    // Empty tenant_id: fine for this single-user local demo tool (matches tools/cli_chat.cpp's and
    // tools/sandboxed_shell_chat.cpp's own established `Principal{"cli-user", ""}` pattern) -- NOT a
    // template for a real multi-tenant host (see that file's own comment for why).
    Principal const cli_principal{"cli-user", ""};
    IdentityHandle const owner = authority.adopt(cli_principal);

    // Generous, demo-appropriate ceilings -- matches tools/sandboxed_shell_chat.cpp's own posture.
    auto branch_quota = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    auto run_quota = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 10'000);
    auto storage_quota =
        agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 100'000'000);
    if (!branch_quota.has_value() || !run_quota.has_value() || !storage_quota.has_value()) {
        std::cerr << "FATAL: failed to mint this session's real sandbox quotas\n";
        return 1;
    }
    auto root_branch = agentengine::rt::block_on(cli_ledger.create_root_branch(owner, session_id));
    if (!root_branch.has_value()) {
        std::cerr << "FATAL: failed to create this session's real sandbox root branch: "
                  << root_branch.error().message << "\n";
        return 1;
    }
    std::filesystem::path const ledger_staging_root =
        std::filesystem::temp_directory_path() / ("ae_containerd_shell_chat_ledger_" + session_id);
    std::filesystem::path const shell_scratch_root =
        std::filesystem::temp_directory_path() / ("ae_containerd_shell_chat_shell_" + session_id);

    // MandatorySandboxProvider's own bind_sandbox() must run on the LOCAL value BEFORE it is moved into
    // .providers() below -- see tools/sandboxed_shell_chat.cpp's own comment for why
    // (ComposedContextProvider<Ms...>'s own descriptor factory type-erases each Ms once engaged, with
    // no accessor back to the concrete instance).
    MandatorySandboxProvider<ContainerdExecutionSurface> sandbox_provider;
    sandbox_provider.bind_sandbox(cli_ledger, std::move(*root_branch), owner, ledger_staging_root,
                                    *branch_quota, *run_quota, *storage_quota);
    SandboxToolProvider shell_provider(shell_scratch_root.string());

    // ---- The session itself, via the already-shipped, already-tested §2b builder ---------------
    using Builder = quickstart::ComposedQuickstartSessionBuilder<
        quickstart::Provider::openai, InMemorySecretStore, SandboxToolProvider,
        MandatorySandboxProvider<ContainerdExecutionSurface>>;
    // `run_shell` (SandboxToolProvider) declares a static Capabilities<> ceiling checked by the real
    // invoke_tool() pipeline -- FsRead/FsWrite scoped to "work", matching tests/test_sandbox_tool_
    // provider.cpp's own real usage. `run_command` (MandatorySandboxProvider) deliberately has NO
    // static ceiling here -- it authorizes through IdentityAuthority/Grant<T>/AsyncQuota<T> instead
    // (ADR-102 Phase 4's own design), so it needs no capability grant.
    auto built = Builder(model)
                     .session_id(session_id)
                     .principal(cli_principal)
                     .grant(Capability{cap::FsRead{"work", "", std::nullopt}})
                     .grant(Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}})
                     .api_key_from_env("openai-api-key", "OPENAI_API_KEY")
                     .providers(std::make_tuple(std::move(shell_provider), std::move(sandbox_provider)))
                     .build();
    if (!built.has_value()) {
        std::cerr << "FATAL: failed to build the session: " << built.error().message << " ("
                  << built.error().code << ")\n";
        if (built.error().code == "quickstart_builder.no_store") {
            std::cerr << "Set OPENAI_API_KEY in the environment before running this tool.\n";
        }
        return 1;
    }

    std::cout << "containerd_shell_chat -- run_shell (native jail) + run_command (containerd-backed, "
                 "identity-native ledger), composed via ComposedContextProvider<Ms...> into one real "
                 "session. Type a message, or 'exit' to quit.\n";

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

    std::cout << "Goodbye.\n";
    // Ordinary return, not std::_Exit() -- see tools/sandboxed_shell_chat.cpp's own file banner:
    // ContainerdExecutionSurface's own destructor reclaims its container normally here. The SAME
    // Ctrl+C caveat that file discloses applies here too (no SIGINT handler installed anywhere in
    // this codebase): an interactive user's Ctrl+C runs the platform's own default signal handler,
    // which does not unwind main()'s stack, so this specific "closes the leak" claim covers ordinary
    // return only -- the ADR-106 §7 orphan-reclaim residual this same session already disclosed.
    return 0;
}
