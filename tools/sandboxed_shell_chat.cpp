// ADR-102, Phase 5 slice ("ComposedContextProvider<Ms...> real production consumer",
// decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md §7's own residual): the first
// real, user-reachable CLI wiring of agentengine::ComposedContextProvider<Ms...> anywhere in this
// codebase, composing TWO real, independently-shipped ContextProvider conformers into ONE session:
//
//   - SandboxToolProvider (ADR-096, src/backends/native_jail/sandbox_tool_provider.hpp) -- `run_shell`,
//     a native OS-level jail rooted at a per-session scratch directory. Had zero real production
//     consumers anywhere in this codebase before this file (only its own test exercised it).
//   - MandatorySandboxProvider<DockerExecutionSurface> (ADR-102 Phases 1-4, include/agentengine/
//     sandbox/mandatory_sandbox_provider.hpp) -- `run_command`, a Docker-backed, identity-native
//     execution surface whose file writes flow through a real, content-addressed `Ledger<>` checkpoint
//     chain. tools/cli_chat.cpp already wires this alone, as a session's BARE HistoryProviderT
//     (deliberately not through ComposedContextProvider<Ms...> -- see that file's own Phase 5 top
//     comment); this tool is the first real host to compose it alongside a SECOND real provider in the
//     same session.
//
// Deliberately a SEPARATE, small tool rather than a change to tools/cli_chat.cpp itself: cli_chat.cpp's
// own HistoryProviderT (ToolDeclaringHistoryProvider) is a large, already-shipped, hand-rolled
// composite (execute_code, mount_skill, skills state, run_command) that stays a session's BARE
// HistoryProviderT specifically so AgentSession::fork_from() keeps compiling for that session type --
// ComposedContextProvider<Ms...> is unconditionally move-only (ADR-074 Finding B), so wrapping
// cli_chat.cpp's own provider in it would make fork_from() a compile error for the flagship
// interactive CLI, a real, avoidable regression this file does not risk. This tool's own session type
// never calls fork_from() (it has no forking/spawn feature at all), so that constraint costs nothing
// here.
//
// Reuses, rather than re-implements, THREE pieces of already-shipped, already-tested machinery that
// had no real production caller before this file: `quickstart::ComposedQuickstartSessionBuilder<
// Provider, Store, Ms...>` (core/session_builder.hpp §2b) for credential/capability/session wiring,
// and `Bundle::ask()` (same file) for the one-shot round-trip REPL loop below -- no hand-rolled
// resume()-loop, unlike cli_chat.cpp's own larger interactive driver, which predates `Bundle` and has
// its own separate reasons to stay hand-rolled (real per-token streaming, CodeAct approval prompts,
// skills banners) that do not apply to this smaller tool.
//
// Exits normally (no std::_Exit()) -- unlike cli_chat.cpp, which forces std::_Exit(0) on its success
// path for an unrelated, already-disclosed CPython Py_Finalize thread-affinity crash (that file's own
// comment). This tool never touches the embedded Python interpreter, so ordinary destruction runs on
// every path THROUGH main()'s own return statements: DockerExecutionSurface's own destructor reclaims
// its container, closing -- for those paths specifically, not cli_chat.cpp -- the container-leak
// residual ADR-102 Phase 5 disclosed (that ADR's §19/§30). NARROWER THAN IT MIGHT READ, disclosed here
// rather than left implicit (ADR-102 §41-43's own red-team round, 2026-08-28): this codebase installs
// no SetConsoleCtrlHandler/SIGINT handler anywhere, in this file or cli_chat.cpp, so an interactive
// user's Ctrl+C while this REPL is blocked on std::getline or a real network call runs Windows' own
// default console handler, which calls ExitProcess() directly -- main()'s stack, and therefore `built`'s
// DockerExecutionSurface, is never unwound. Ctrl+C is an entirely ordinary way a user ends a session
// here, not an edge case, so this file's own "closes the leak" claim covers ordinary return only, not
// every real way this process can end. A real SIGINT handler is contained, disclosed follow-on work,
// not attempted in this pass.
//
// ADR-108 §7 residual, closed here: main() now runs `DockerCliBackend::reap_orphans()` as an explicit,
// best-effort startup sweep before building the session -- closing containers a PRIOR run of this
// exact tool orphaned (a crash, a SIGKILL, or the ordinary-exit destructor-time `docker rm -f` failure
// this file's own comment above already discloses). This is exactly the case the Ctrl+C caveat above
// leaves unclosed on its OWN run -- the sweep only ever finds and reaps what an EARLIER invocation
// left behind, never the current one (which is still live while this sweep runs).
//
// REQUIRES: Windows, a running Docker daemon reachable via the `docker` CLI on PATH, and
// OPENAI_API_KEY set in the environment. NOT for the reason this comment previously gave
// (SandboxToolProvider's own platform scope -- stale as of ADR-103): decisions/ADR-105-sandbox-tool-
// provider-composed-linux-parity.md traced the real Linux blocker to this file's use of
// QuarantineSecretStore (trust/secret_quarantine.hpp), which needs agentengine::trust::hmac_sha256 --
// a symbol with no Linux implementation at all (ADR-005's own deliberately Windows-only BCrypt/CNG
// scope), unrelated to SandboxToolProvider.

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/session_builder.hpp"
#include "agentengine/sandbox/docker_execution_surface.hpp"
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

    // ADR-108 §7 residual, closed here: an explicit, best-effort startup sweep for containers this
    // same naming scheme orphaned on a PRIOR run of this tool (a crash, a SIGKILL, or an ordinary exit
    // whose destructor-time `docker rm -f` transiently failed). Deliberately non-fatal -- a daemon
    // that's briefly unreachable or a sweep that finds nothing must never block this tool's own real
    // purpose (the interactive session below); only reported, never treated as an error.
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
    std::string const session_id = "sandboxed-shell-chat-session";

    // ---- The identity-native sandbox: ledger/authority/quotas/root branch (ADR-102 Phases 1-4) -----
    // `cli_ledger` is declared first and never destroyed before `sandbox_provider`/the built session
    // below -- `BranchHandle::~BranchHandle()` (core/ledger.hpp) dereferences a raw `Ledger<>*` back to
    // it, the same lifetime-ordering requirement tools/cli_chat.cpp's own Phase 5 wiring documents.
    // This whole function is one scope with a single return path per branch, so C++'s own
    // reverse-declaration-order destruction is sufficient here without a separate comment per exit.
    Ledger<> cli_ledger;
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    // Empty tenant_id: fine for this single-user local demo tool (matches tools/cli_chat.cpp's own
    // established `Principal{"cli-user", ""}` pattern) -- NOT a template for a real multi-tenant host.
    // ADR-102 Phase 1's own central fix (§4) was specifically about IdentityAuthority::adopt() keying
    // on (tenant_id, id) rather than id alone, to prevent two tenants' principals sharing an id string
    // from silently merging into one durable identity; copying this literal empty-tenant pattern into
    // a multi-user host would reintroduce exactly that collision class.
    Principal const cli_principal{"cli-user", ""};
    IdentityHandle const owner = authority.adopt(cli_principal);

    // Generous, demo-appropriate ceilings -- this tool runs exactly one interactive session per
    // process, the same posture tools/cli_chat.cpp's own Phase 5 wiring already takes.
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
        std::filesystem::temp_directory_path() / ("ae_sandboxed_shell_chat_ledger_" + session_id);
    std::filesystem::path const shell_scratch_root =
        std::filesystem::temp_directory_path() / ("ae_sandboxed_shell_chat_shell_" + session_id);

    // MandatorySandboxProvider's own bind_sandbox() must run on the LOCAL value BEFORE it is moved into
    // .providers() below -- ComposedContextProvider<Ms...>'s own descriptor factory type-erases each
    // Ms into a shared_ptr<Ms> once engaged, with no accessor back to the concrete instance (see
    // tests/test_composed_sandbox_providers_live.cpp's own file-top comment for the full reasoning).
    MandatorySandboxProvider<DockerExecutionSurface> sandbox_provider;
    sandbox_provider.bind_sandbox(cli_ledger, std::move(*root_branch), owner, ledger_staging_root,
                                    *branch_quota, *run_quota, *storage_quota);
    SandboxToolProvider shell_provider(shell_scratch_root.string());

    // ---- The session itself, via the already-shipped, already-tested §2b builder ---------------
    using Builder = quickstart::ComposedQuickstartSessionBuilder<
        quickstart::Provider::openai, InMemorySecretStore, SandboxToolProvider,
        MandatorySandboxProvider<DockerExecutionSurface>>;
    // `run_shell` (SandboxToolProvider) declares a static Capabilities<> ceiling checked by the real
    // invoke_tool() pipeline -- FsRead/FsWrite scoped to "work", matching tests/test_sandbox_tool_
    // provider.cpp's own real usage. `run_command` (MandatorySandboxProvider) authorizes through
    // IdentityAuthority/Grant<T>/AsyncQuota<T> (ADR-102 Phase 4's own design, unchanged) PLUS, as of
    // ADR-119, a real static Capabilities<cap::decl::RunCommand> ceiling layered on top -- granted
    // explicitly below, or every real run_command call would now be rejected by invoke_tool() step
    // 4/7 even though the identity/quota gate would have allowed it.
    auto built = Builder(model)
                     .session_id(session_id)
                     .principal(cli_principal)
                     .grant(Capability{cap::FsRead{"work", "", std::nullopt}})
                     .grant(Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}})
                     .grant(Capability{cap::RunCommand{}})
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

    std::cout << "sandboxed_shell_chat -- run_shell (native jail) + run_command (Docker-backed, "
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
    // Ordinary return, not std::_Exit() -- see file banner: DockerExecutionSurface's own destructor
    // reclaims its container normally here.
    return 0;
}
