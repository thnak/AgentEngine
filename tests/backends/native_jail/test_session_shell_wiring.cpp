// Phase 1 of the "every session gets a real sandbox" roadmap
// (C:\Users\thanh\.claude\plans\resilient-yawning-cook.md). Proves core/session_shell_wiring.hpp's
// SessionShellSandbox end to end: a real ToolDescriptor for "run_shell", built via
// make_tool_descriptor_with_invoke (ADR-028), resolved through the REAL 006 §3 ten-step pipeline
// (invoke_tool, core/tool_pipeline.hpp) -- not a bypass -- against a real host directory. Also
// proves the one place Phase 0 (EffectContext::sandbox_fs) and Phase 1 connect: the SAME
// FileSystemAdapter this sandbox constructs for run_shell also populates ctx.sandbox_fs, reachable
// by an ordinary native Tool independent of the shell tool itself.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "backends/native_jail/session_shell_wiring.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

agentengine::EffectContext make_ctx(agentengine::CapabilitySet const& held) {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);
    return ctx;
}

}  // namespace

int main() {
    namespace json = agentengine::json;
    using agentengine::CapabilitySet;
    using agentengine::SessionShellSandbox;
    using agentengine::ToolCallRequest;
    using agentengine::ToolTable;
    using agentengine::invoke_tool;

    // std::filesystem::temp_directory_path() (portable -- TEMP/TMP on Windows, TMPDIR/"/tmp" on
    // Linux) rather than a hand-read TEMP env var with a Windows-only fallback path (2026-08-28,
    // ADR-103, the Linux-parity pass).
    std::filesystem::path const scratch =
        std::filesystem::temp_directory_path() / "ae_session_shell_wiring_test";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    auto sandbox = SessionShellSandbox::create(scratch);
    check(sandbox.has_value(), "setup: SessionShellSandbox::create succeeds against a real directory");
    if (!sandbox) {
        std::fprintf(stderr, "test_session_shell_wiring: setup failed, cannot continue\n");
        return 1;
    }

    auto const table = ToolTable::from_descriptors({(*sandbox)->tool_descriptor()});

    // -- Positive case: granted capabilities, a real script that writes a file --------------------
    {
        CapabilitySet const held = CapabilitySet::grant_root({
            agentengine::Capability{agentengine::cap::FsRead{"work", "", std::nullopt}},
            agentengine::Capability{agentengine::cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
        });
        auto ctx = make_ctx(held);
        // Phase 0 <-> Phase 1 connection: the same mediated adapter run_shell uses also reaches the
        // session's own EffectContext::sandbox_fs seam, independent of the shell tool itself.
        ctx.sandbox_fs = (*sandbox)->filesystem_adapter();
        check(ctx.sandbox_fs != nullptr, "ctx.sandbox_fs is populated from the same live adapter");

        ToolCallRequest const req{
            "call-1", "run_shell",
            json::Value::make_object(
                {{"source", json::Value::make_string("echo written-by-shell > out.txt")}}),
            false};
        agentengine::ToolInvocationAudit audit;
        auto result = invoke_tool(table, held, req, ctx, nullptr, &audit);
        check(!result.is_error, "granted capabilities: run_shell call succeeds");
        check(audit.ok, "audit records success");
        if (!result.content.empty()) {
            auto const* data = std::get_if<agentengine::Data>(&result.content[0].value);
            check(data != nullptr, "success result is a Data content item");
            if (data) {
                auto parsed = json::parse(data->json);
                check(parsed.has_value() && parsed->find("ok")->as_bool(),
                      "reply reports ok:true for a successful command");
            }
            check(result.content[0].tainted, "tool results are provenance-marked tainted (006 §7)");
        }

        // The write actually landed on the REAL host filesystem -- not just trusted from the tool's
        // own report.
        std::ifstream in(scratch / "out.txt");
        std::string line;
        std::getline(in, line);
        check(in.good() || !line.empty(), "the file the shell wrote is readable directly from the host fs");
        check(line == "written-by-shell", "the file's real content matches what the shell command wrote");
    }

    // -- Negative case: capability-denied never bypasses the real pipeline -------------------------
    {
        CapabilitySet const held;  // empty -- no FsRead/FsWrite grant at all
        auto ctx = make_ctx(held);
        ToolCallRequest const req{
            "call-2", "run_shell",
            json::Value::make_object({{"source", json::Value::make_string("echo should_not_run > denied.txt")}}),
            false};
        auto result = invoke_tool(table, held, req, ctx, nullptr);
        check(result.is_error, "no granted capability: run_shell call is denied");
        auto const* err = std::get_if<agentengine::Error>(&result.content[0].value);
        check(err != nullptr, "denial is a structured Error content item, not a crash or silent no-op");

        check(!std::filesystem::exists(scratch / "denied.txt"),
              "a denied call never reaches the real shell -- no file was written");
    }

    // -- State persistence across calls within one session (cd/export survive) ---------------------
    {
        CapabilitySet const held = CapabilitySet::grant_root({
            agentengine::Capability{agentengine::cap::FsRead{"work", "", std::nullopt}},
            agentengine::Capability{agentengine::cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
        });
        auto ctx1 = make_ctx(held);
        ToolCallRequest const mkdir_req{
            "call-3a", "run_shell",
            json::Value::make_object({{"source", json::Value::make_string("mkdir sub")}}), false};
        auto mkdir_result = invoke_tool(table, held, mkdir_req, ctx1, nullptr);
        check(!mkdir_result.is_error, "mkdir sub succeeds");

        auto ctx2 = make_ctx(held);
        ToolCallRequest const cd_req{
            "call-3b", "run_shell",
            json::Value::make_object({{"source", json::Value::make_string("cd sub")}}), false};
        auto cd_result = invoke_tool(table, held, cd_req, ctx2, nullptr);
        check(!cd_result.is_error, "cd sub succeeds");

        auto ctx3 = make_ctx(held);
        ToolCallRequest const pwd_req{
            "call-3c", "run_shell",
            json::Value::make_object({{"source", json::Value::make_string("pwd")}}), false};
        auto pwd_result = invoke_tool(table, held, pwd_req, ctx3, nullptr);
        check(!pwd_result.is_error, "pwd succeeds");
        if (!pwd_result.content.empty()) {
            auto const* data = std::get_if<agentengine::Data>(&pwd_result.content[0].value);
            if (data) {
                auto parsed = json::parse(data->json);
                auto stdout_text = parsed->find("stdout_text");
                check(stdout_text != nullptr && stdout_text->as_string().find("sub") != std::string::npos,
                      "cwd from an earlier call (call-3b) is still in effect for this later call "
                      "(call-3c) -- ExecState genuinely persists across calls within this session");
            }
        }
    }

    std::filesystem::remove_all(scratch);

    if (g_failures == 0) {
        std::printf("test_session_shell_wiring: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_session_shell_wiring: %d check(s) failed\n", g_failures);
    return 1;
}
