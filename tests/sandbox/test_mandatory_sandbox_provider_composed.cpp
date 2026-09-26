// Proves ADR-102 Phase 5's own real composition pattern -- `tools/cli_chat.cpp`'s
// `ToolDeclaringHistoryProvider` embeds `MandatorySandboxProvider<Surface>` as a member and merges its
// `on_context()` contribution into its own, OUTSIDE any skill-scoping mechanism (`run_command` is a
// session-level sandbox capability, not a skill-unlocked one). This exact composition -- one
// `ContextProvider` embedding another and folding its contribution into its own -- has no automated
// coverage anywhere else: `tests/test_mandatory_sandbox_provider.cpp` only ever exercises
// `MandatorySandboxProvider` as the SOLE `HistoryProviderT`, never composed alongside a second provider
// the way `cli_chat.cpp` actually does it; `cli_chat.cpp` itself is not unit-testable in isolation (it
// needs `AGENTENGINE_WITH_HTTPS`/`AGENTENGINE_BUILD_PYTHON_RUNNER` and a live model backend). This file
// closes that real gap with a minimal, Docker-independent stand-in: `ComposedProvider` mirrors
// `ToolDeclaringHistoryProvider`'s own shape (one always-on local tool it declares directly, plus an
// embedded `MandatorySandboxProvider<FakeSurface>` merged in outside any scoping), proving the
// composition PATTERN itself, not `run_command`'s own real execution (already proven, against a REAL
// Docker daemon, by test_mandatory_sandbox_provider.cpp and test_sandbox_runtime.cpp).
//
//   [1] before bind_sandbox(), on_context() contributes ONLY the composing provider's own local tool.
//   [2] immediately after bind_sandbox() -- on the VERY FIRST on_context() call, not a later one --
//       BOTH tools are present: the composing provider's own, and run_command, merged in from the
//       embedded MandatorySandboxProvider.
//   [3] after a real fork (MandatorySandboxProvider's own copy-assignment), the forked composed
//       provider's on_context() still contributes both tools, from a genuinely independent branch.

#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// A trivial ExecutionSurface stand-in -- never actually reset()/run()/drain_to()'d by this file (no
// check here invokes run_command's real closure, only checks its DECLARATION), so it never touches a
// real process or Docker. Exists only to satisfy MandatorySandboxProvider<Surface>'s own template
// constraint with something real, matching the ExecutionSurface concept exactly.
struct FakeSurface {
    [[nodiscard]] agentengine::result<void> reset(std::filesystem::path const&) {
        return agentengine::result<void>{};
    }
    [[nodiscard]] agentengine::result<SurfaceRunOutcome> run(std::string const&) {
        return SurfaceRunOutcome{0, ""};
    }
    [[nodiscard]] agentengine::result<void> drain_to(std::filesystem::path const&) {
        return agentengine::result<void>{};
    }
};
static_assert(ExecutionSurface<FakeSurface>);

struct LocalToolArgs { bool unused = false; };
AE_JSON_SCHEMA(LocalToolArgs, unused)
struct LocalToolReply { bool ok = true; };
AE_JSON_SCHEMA(LocalToolReply, ok)

struct LocalTool : Tool<LocalTool> {
    static constexpr std::string_view name = "local_tool";
    static constexpr std::string_view description = "The composing provider's own always-on tool.";
    using Args = LocalToolArgs;
    using Reply = LocalToolReply;
    [[nodiscard]] static result<Reply> invoke(Args, EffectContext&) { return Reply{}; }
};

// Mirrors ToolDeclaringHistoryProvider's own real shape (tools/cli_chat.cpp): one local tool this
// provider declares directly, plus an embedded MandatorySandboxProvider whose own contribution is
// merged in unconditionally, outside any scoping -- the exact pattern this file exists to prove.
class ComposedProvider {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext& ec) {
        ContextContribution contribution;
        contribution.tools.push_back(make_tool_descriptor<LocalTool>());

        auto run_command_contribution = co_await run_command_provider_.on_context(sc, ec);
        if (!run_command_contribution) co_return std::unexpected(run_command_contribution.error());
        for (ToolDescriptor& td : run_command_contribution->tools) {
            contribution.tools.push_back(std::move(td));
        }
        co_return contribution;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

    [[nodiscard]] MandatorySandboxProvider<FakeSurface>& run_command_provider() noexcept {
        return run_command_provider_;
    }

private:
    MandatorySandboxProvider<FakeSurface> run_command_provider_;
};
static_assert(ContextProvider<ComposedProvider>);

[[nodiscard]] bool has_tool(ContextContribution const& c, std::string_view name) {
    for (ToolDescriptor const& td : c.tools) {
        if (td.name == name) return true;
    }
    return false;
}

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal const real_principal = make_embedded_principal("composed-provider-test-owner");
    IdentityHandle owner = authority.adopt(real_principal);
    Ledger<> ledger;
    auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    check(storage_quota_r.has_value(), "AsyncQuota<StorageBytes>::mint_root(owner) succeeds");
    if (!storage_quota_r.has_value()) return EXIT_FAILURE;
    auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    check(run_quota_r.has_value(), "AsyncQuota<RunCost>::mint_root(owner) succeeds");
    if (!run_quota_r.has_value()) return EXIT_FAILURE;
    auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    check(branch_quota_r.has_value(), "AsyncQuota<BranchCost>::mint_root(owner) succeeds");
    if (!branch_quota_r.has_value()) return EXIT_FAILURE;

    std::vector<Message> empty_history;
    SessionContext session_ctx{"composed-provider-test", real_principal, empty_history};
    EffectContext ctx;
    ctx.principal = real_principal;
    CapabilitySet const caps = CapabilitySet::grant_root({});
    ctx.capabilities = borrow_capabilities(caps);

    ComposedProvider provider;

    // [1] before bind_sandbox(): only the composing provider's own local tool.
    {
        auto contribution = drive(provider.on_context(session_ctx, ctx));
        check(contribution.has_value(), "on_context() before bind_sandbox() succeeds");
        if (contribution.has_value()) {
            check(has_tool(*contribution, "local_tool"), "local_tool is present before bind_sandbox()");
            check(!has_tool(*contribution, "run_command"),
                  "run_command is NOT present before bind_sandbox()");
            check(contribution->tools.size() == 1, "exactly one tool contributed before bind_sandbox()");
        }
    }

    // [2] immediately after bind_sandbox(), on the VERY FIRST on_context() call: both tools present.
    {
        auto root_r = drive(ledger.create_root_branch(owner, "composed"));
        check(root_r.has_value(), "create_root_branch() succeeds");
        if (root_r.has_value()) {
            std::filesystem::path const staging =
                std::filesystem::temp_directory_path() / "ae_test_composed_provider";
            std::error_code ec;
            std::filesystem::remove_all(staging, ec);
            provider.run_command_provider().bind_sandbox(ledger, std::move(*root_r), owner, staging,
                                                             *branch_quota_r, *run_quota_r, *storage_quota_r);
            check(provider.run_command_provider().is_bound(), "bind_sandbox() leaves the provider bound");

            auto contribution = drive(provider.on_context(session_ctx, ctx));
            check(contribution.has_value(),
                  "on_context() on the very first call after bind_sandbox() succeeds");
            if (contribution.has_value()) {
                check(has_tool(*contribution, "local_tool"),
                      "local_tool is still present after bind_sandbox()");
                check(has_tool(*contribution, "run_command"),
                      "run_command is present on the VERY FIRST on_context() call after bind_sandbox() "
                      "-- the exact real-conversation gap an earlier stale-binary confusion during this "
                      "session's own manual smoke testing surfaced (traced to a masked ninja build "
                      "failure, not a logic defect -- but genuinely uncovered by any automated test "
                      "until this file)");
                check(contribution->tools.size() == 2,
                      "exactly two tools contributed once bound: local_tool + run_command");
            }
        }
    }

    // [3] after a real fork (MandatorySandboxProvider's own copy-assignment), both tools still present.
    {
        MandatorySandboxProvider<FakeSurface> forked_runtime(provider.run_command_provider());
        check(forked_runtime.is_bound(), "a forked (copy-constructed) provider is bound");
        ComposedProvider forked;
        forked.run_command_provider() = forked_runtime;
        auto contribution = drive(forked.on_context(session_ctx, ctx));
        check(contribution.has_value(), "on_context() on a forked composed provider succeeds");
        if (contribution.has_value()) {
            check(has_tool(*contribution, "local_tool"), "local_tool present after fork");
            check(has_tool(*contribution, "run_command"), "run_command present after fork");
        }
    }

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- the real cli_chat.cpp composition pattern (one ContextProvider "
                     "embedding MandatorySandboxProvider and merging its contribution outside any "
                     "skill-scoping) contributes run_command correctly from the very first on_context() "
                     "call after bind_sandbox(), including across a real fork.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
