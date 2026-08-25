// ADR-096 (decisions/ADR-096-session-sandbox-lifecycle-context-provider-wiring.md), Design B:
// proves `SandboxToolProvider` (backends/native_jail/sandbox_tool_provider.hpp) end to end --
// composition (C1), lazy sandbox construction + `ctx.sandbox_fs` population + `run_shell` tool
// contribution on `on_context()`, idempotent reuse across a second round (matching C3's premise
// that nothing here needs to survive `fork_from()`/`clear_in_process_state()` to work), the real
// `invoke_tool()` pipeline actually running a command against the created directory, and the
// digest-based subdirectory naming's defense-in-depth check (C8).

#include <cstdio>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/pal/env.hpp"
#include "backends/native_jail/sandbox_tool_provider.hpp"
#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

agentengine::EffectContext make_ctx(agentengine::CapabilitySet const& held) {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);
    return ctx;
}

}  // namespace

// -- Compile-time proofs (C1, C2's premise) -------------------------------------------------------
static_assert(agentengine::ContextProvider<agentengine::SandboxToolProvider>,
              "SandboxToolProvider must satisfy ContextProvider (005 §5) to be Ms...-composable "
              "with no new AgentSession mutator (ADR-096 C1)");
static_assert(!std::is_copy_constructible_v<agentengine::SandboxToolProvider> &&
                  !std::is_copy_assignable_v<agentengine::SandboxToolProvider>,
              "non-copyable by construction (owns a unique_ptr<SessionShellSandbox>) -- composing "
              "this makes AgentSession::fork_from() fail to compile, not alias live sandbox state "
              "across sessions (ADR-096 C2)");
static_assert(std::is_move_constructible_v<agentengine::SandboxToolProvider>,
              "still move-constructible -- ComposedContextProvider<Ms...>'s own internal moves "
              "(and AgentSession::clear_in_process_state()) must keep working");

using ComposedSandbox = agentengine::ComposedContextProvider<agentengine::SandboxToolProvider>;
static_assert(agentengine::ContextProvider<ComposedSandbox>,
              "ComposedContextProvider<SandboxToolProvider> must itself satisfy ContextProvider");

int main() {
    namespace json = agentengine::json;
    using agentengine::SandboxToolProvider;

    std::string const scratch_root = agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
                                      "/ae_sandbox_tool_provider_test";
    std::filesystem::remove_all(scratch_root);
    // Deliberately do NOT create scratch_root itself -- proves the provider's own idempotent
    // create_directories() (ADR-096 §8 residual) handles a root that does not exist yet.

    agentengine::Principal const principal{"p-sandbox-provider", ""};
    std::string const session_id = "session-abc-123";

    // Independently derive the expected per-session subdirectory name the same way the provider
    // does internally, so the test can assert against a real, predictable host path rather than
    // just "some directory got created somewhere."
    std::vector<std::byte> id_bytes(session_id.size());
    for (std::size_t i = 0; i < session_id.size(); ++i) {
        id_bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(session_id[i]));
    }
    auto expected_digest = agentengine::compute_digest(id_bytes);
    check(expected_digest.has_value(), "setup: compute_digest succeeds for a plain session id");
    std::filesystem::path const expected_session_dir =
        std::filesystem::path(scratch_root) / (expected_digest ? *expected_digest : "");

    // -- Part 1: first on_context() lazily creates the sandbox, contributes run_shell, and --------
    //    populates ctx.sandbox_fs (ADR-096 §2 Design B's central claim) -------------------------
    SandboxToolProvider provider(scratch_root);
    agentengine::ToolDescriptor first_descriptor;
    {
        agentengine::EffectContext ctx = make_ctx(agentengine::CapabilitySet::grant_root({}));
        std::vector<agentengine::Message> history;
        agentengine::SessionContext session_ctx{session_id, principal, history};

        check(ctx.sandbox_fs == nullptr, "before on_context(): ctx.sandbox_fs is the default nullptr");

        auto out = agentengine::test_support::run_task_sync<
            agentengine::result<agentengine::ContextContribution>>(provider.on_context(session_ctx, ctx));

        check(out.has_value(), "Part 1: first on_context() succeeds");
        check(out.has_value() && out->tools.size() == 1 && out->tools[0].name == "run_shell",
              "Part 1: contributes exactly the run_shell ToolDescriptor");
        check(ctx.sandbox_fs != nullptr,
              "Part 1: ctx.sandbox_fs is populated from the lazily-constructed sandbox");
        check(std::filesystem::exists(expected_session_dir),
              "Part 1: the per-session scratch directory was created idempotently, named by the "
              "hex digest of session_id (ADR-096 C8), not the raw session_id itself");
        if (out.has_value() && !out->tools.empty()) first_descriptor = out->tools[0];
    }

    // -- Part 2: the contributed descriptor runs through the REAL invoke_tool() pipeline ----------
    {
        auto const table = agentengine::ToolTable::from_descriptors({first_descriptor});
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root({
            agentengine::Capability{agentengine::cap::FsRead{"work", "", std::nullopt}},
            agentengine::Capability{agentengine::cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
        });
        auto ctx = make_ctx(held);
        agentengine::ToolCallRequest const req{
            "call-1", "run_shell",
            json::Value::make_object(
                {{"source", json::Value::make_string("echo from-provider > out.txt")}}),
            false};
        agentengine::ToolInvocationAudit audit;
        auto result = agentengine::invoke_tool(table, held, req, ctx, nullptr, &audit);
        check(!result.is_error, "Part 2: run_shell, invoked through the pipeline, succeeds");
        check(audit.ok, "Part 2: audit records success");

        std::ifstream in((expected_session_dir / "out.txt").string());
        std::string line;
        std::getline(in, line);
        check(line == "from-provider",
              "Part 2: the file the shell wrote is readable directly from the real host filesystem, "
              "at the digest-named per-session directory");
    }

    // -- Part 3: a SECOND on_context() call (a later round) reuses the SAME sandbox, does not ------
    //    recreate it, and the shell's ExecState still shows the file from Part 2 -----------------
    {
        agentengine::EffectContext ctx = make_ctx(agentengine::CapabilitySet::grant_root({}));
        std::vector<agentengine::Message> history;
        agentengine::SessionContext session_ctx{session_id, principal, history};

        auto out = agentengine::test_support::run_task_sync<
            agentengine::result<agentengine::ContextContribution>>(provider.on_context(session_ctx, ctx));

        check(out.has_value(), "Part 3: second on_context() (later round) succeeds");
        check(ctx.sandbox_fs != nullptr, "Part 3: ctx.sandbox_fs is populated again on the later round");

        agentengine::ToolTable const table =
            agentengine::ToolTable::from_descriptors({out.has_value() && !out->tools.empty()
                                                            ? out->tools[0]
                                                            : agentengine::ToolDescriptor{}});
        agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root({
            agentengine::Capability{agentengine::cap::FsRead{"work", "", std::nullopt}},
            agentengine::Capability{agentengine::cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
        });
        auto call_ctx = make_ctx(held);
        agentengine::ToolCallRequest const req{
            "call-2", "run_shell",
            json::Value::make_object({{"source", json::Value::make_string("cat out.txt")}}), false};
        auto result = agentengine::invoke_tool(table, held, req, call_ctx, nullptr);
        check(!result.is_error, "Part 3: a later round's run_shell call succeeds against the SAME "
                                 "sandbox (no new directory was created for it)");
        if (!result.content.empty()) {
            auto const* data = std::get_if<agentengine::Data>(&result.content[0].value);
            if (data) {
                auto parsed = json::parse(data->json);
                auto stdout_text = parsed->find("stdout_text");
                check(stdout_text != nullptr &&
                          stdout_text->as_string().find("from-provider") != std::string::npos,
                      "Part 3: Part 2's file is still there and reachable -- same sandbox, "
                      "same host directory, across two separate on_context() rounds");
            }
        }
    }

    // -- Part 4: the digest-validation helper itself (ADR-096 C8 / WT-6 precedent) -----------------
    {
        using agentengine::sandbox_tool_provider_detail::check_session_digest;
        std::string const valid_64(64, 'a');  // a real digest's own length, all-valid hex chars
        std::string const wrong_length(63, 'a');
        std::string uppercase_64 = valid_64;
        uppercase_64[0] = 'A';
        std::string non_hex_64 = valid_64;
        non_hex_64[0] = '!';

        check(check_session_digest("").has_value() == false, "Part 4: an empty digest is rejected");
        check(check_session_digest(wrong_length).has_value() == false,
              "Part 4: a digest of the wrong length is rejected");
        check(check_session_digest(uppercase_64).has_value() == false,
              "Part 4: an uppercase-hex digest is rejected -- only lowercase [0-9a-f] is accepted");
        check(check_session_digest(non_hex_64).has_value() == false,
              "Part 4: a non-hex character is rejected");
        check(check_session_digest(valid_64).has_value(),
              "Part 4: a real 64-char lowercase-hex string is accepted");
        check(expected_digest.has_value() && expected_digest->size() == 64 &&
                  check_session_digest(*expected_digest).has_value(),
              "Part 4: a REAL compute_digest() output passes this same check (the front-door path)");
    }

    std::filesystem::remove_all(scratch_root);

    if (g_failures == 0) {
        std::printf("test_sandbox_tool_provider: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_sandbox_tool_provider: %d check(s) failed\n", g_failures);
    return 1;
}
