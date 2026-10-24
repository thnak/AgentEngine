// Design -> red-team -> prove -> judge for decisions/archive/ADR-071-native-unsandboxed-process-execution-
// providers.md's src/backends/native_process/native_providers.hpp: NativeShellProvider/
// NativeBashProvider/NativePythonProvider/NativeNodeProvider end to end -- concept satisfaction,
// capability-gated discovery, dispatch that starts in the worktree (R-S8 pins what that does NOT
// confine), and the fail-closed defaults ADR-070's Delegated Decision Seam pattern requires
// (explicit opt-in, fails closed when unset, narrows already-possessed authority only, host code
// only, always audited). Every negative-result claim
// below is paired with a positive control (022 §5).

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/pal/env.hpp"
#include "backends/native_process/native_providers.hpp"
#include "../../support/admitted_invoke.hpp"
#include "../../support/run_task_sync.hpp"

using namespace agentengine;
using namespace agentengine::native_process;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

std::wstring widen(std::string const& utf8) {
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

void touch(std::filesystem::path const& p) { std::ofstream(p).put('x'); }

std::string system_dir() {
    char buf[MAX_PATH]{};
    UINT len = GetSystemDirectoryA(buf, MAX_PATH);
    return std::string(buf, len);
}

result<ContextContribution> on_context_of(NativeShellProvider& provider, SessionContext& sc, EffectContext& ctx) {
    return test_support::run_task_sync<result<ContextContribution>>(provider.on_context(sc, ctx));
}

using ae_test::invoke_admitted;

}  // namespace

int main() {
    std::string const saved_path = agentengine::pal::env_var("PATH").value_or("");

    std::filesystem::path const scratch_path =
        std::filesystem::temp_directory_path() / "ae_native_providers_test_scratch";
    std::filesystem::path const worktree_path =
        std::filesystem::temp_directory_path() / "ae_native_providers_test_worktree";
    std::error_code ec;
    std::filesystem::remove_all(scratch_path, ec);
    std::filesystem::remove_all(worktree_path, ec);
    std::filesystem::create_directories(scratch_path);
    std::filesystem::create_directories(worktree_path);
    touch(scratch_path / "node.exe");  // a fake, non-executable "node" -- discovery-only fixtures
    _putenv_s("PATH", (scratch_path.string() + ";" + system_dir()).c_str());

    std::wstring const mount_root = widen(worktree_path.string());
    std::vector<Message> const empty_history;
    Principal const principal{};

    // ---- T1: on_context() with NO capability grant contributes NOTHING (fails closed when unset,
    // ---- ADR-070 property 2) ------------------------------------------------------------------
    {
        NativeShellProvider provider({"cmd"}, mount_root, "workdir");
        EffectContext ctx{.deadline = agentengine::Deadline::none()};  // capabilities left null -- no grant at all
        SessionContext sc{"s1", principal, empty_history};
        auto contribution = on_context_of(provider, sc, ctx);
        AE_CHECK(contribution.has_value(), "T1: on_context() itself does not error with no grant");
        if (contribution.has_value()) {
            AE_CHECK(contribution->tools.empty(), "T1: no tools contributed with no held grant");
            AE_CHECK(!contribution->instructions.has_value(),
                      "T1: no instructions seeded with no held grant");
        }
        AE_CHECK(!provider.is_available(ctx), "T1: is_available() is false with no held grant");
    }

    // ---- T2 (positive control): a held, matching grant seeds instructions + exactly one tool ----
    {
        NativeShellProvider provider({"cmd"}, mount_root, "workdir");
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"cmd", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}}));
        SessionContext sc{"s2", principal, empty_history};
        AE_CHECK(provider.is_available(ctx), "T2: is_available() is true with a held, matching grant");
        auto contribution = on_context_of(provider, sc, ctx);
        AE_CHECK(contribution.has_value(), "T2: on_context() succeeds with a held grant");
        if (contribution.has_value()) {
            AE_CHECK(contribution->tools.size() == 1, "T2: exactly one tool contributed");
            AE_CHECK(contribution->tools[0].name == "native_shell_run", "T2: the tool's name is native_shell_run");
            AE_CHECK(contribution->instructions.has_value(), "T2: instructions ARE seeded");
            if (contribution->instructions.has_value()) {
                AE_CHECK(contribution->instructions->unsafe_view().find("cmd") != std::string::npos,
                          "T2: seeded instructions actually name the discovered executable");
            }
        }
    }

    // ---- R-T3: a grant for an UNOWNED pattern (not in this provider's own owned_patterns) is ------
    // ---- never used, even though it is a real, held cap::NativeExec grant ------------------------
    {
        NativeShellProvider provider({"cmd"}, mount_root, "workdir");  // owns "cmd" only
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"node", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}}));
        AE_CHECK(!provider.is_available(ctx),
                  "R-T3: a real grant for a DIFFERENT (unowned) pattern does not make this provider "
                  "available");
    }

    // ---- R-T4: a grant with a MISMATCHED worktree_mount_id is never used, even if the program -----
    // ---- pattern matches exactly (defense in depth against a wiring mistake) ---------------------
    {
        NativeShellProvider provider({"cmd"}, mount_root, "workdir");
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"cmd", "OTHER_MOUNT", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}}));
        AE_CHECK(!provider.is_available(ctx),
                  "R-T4: a program-name match against the WRONG worktree_mount_id is not available");
    }

    // ---- S5 (positive control): a real end-to-end invoke through the tool's own closure -----------
    {
        NativeShellProvider provider({"cmd"}, mount_root, "workdir");
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"cmd", "workdir", cap::Limit::at_most(10000), cap::Limit::at_most(10000), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}}));
        SessionContext sc{"s5", principal, empty_history};
        auto contribution = on_context_of(provider, sc, ctx);
        AE_CHECK(contribution.has_value() && contribution->tools.size() == 1,
                  "S5 setup: exactly one tool contributed");
        if (contribution.has_value() && contribution->tools.size() == 1) {
            auto args_json = json::Value::make_object(
                {{"program", json::Value::make_string("cmd")},
                 {"args", json::Value::make_array(
                              {json::Value::make_string("/c"), json::Value::make_string("echo"),
                               json::Value::make_string("hello-native-provider")})}});
            auto reply_json = invoke_admitted(contribution->tools[0], args_json, ctx);
            AE_CHECK(reply_json.has_value(), "S5: the tool's own closure spawns for real and succeeds");
            if (reply_json.has_value()) {
                auto reply = schema::from_json<NativeProcessRunReply>(*reply_json);
                AE_CHECK(reply.has_value(), "S5: the reply JSON round-trips through the declared schema");
                if (reply.has_value()) {
                    AE_CHECK(reply->exit_code == 0, "S5: exit code is 0");
                    AE_CHECK(reply->stdout_text.find("hello-native-provider") != std::string::npos,
                              "S5: stdout actually contains the echoed text");
                }
            }
        }
    }

    // ---- R-S6: the SAME tool closure, called with NO capabilities set at all, is denied -----------
    // ---- (per-invocation re-check, not trusting on_context()'s earlier scan) ----------------------
    {
        NativeShellProvider provider({"cmd"}, mount_root, "workdir");
        EffectContext granted_ctx{.deadline = agentengine::Deadline::none()};
        granted_ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"cmd", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}}));
        SessionContext sc{"s6", principal, empty_history};
        auto contribution = on_context_of(provider, sc, granted_ctx);
        AE_CHECK(contribution.has_value() && contribution->tools.size() == 1, "R-S6 setup");

        EffectContext ungranted_ctx{.deadline = agentengine::Deadline::none()};  // deliberately no capabilities -- simulates a revoked/never-held grant
        auto args_json = json::Value::make_object(
            {{"program", json::Value::make_string("cmd")}, {"args", json::Value::make_array({})}});
        if (contribution.has_value() && contribution->tools.size() == 1) {
            auto reply_json = invoke_admitted(contribution->tools[0], args_json, ungranted_ctx);
            AE_CHECK(!reply_json.has_value(),
                      "R-S6: invoking with an EffectContext that holds no NativeExec grant at all is "
                      "denied, even using a ContextContribution built while a grant WAS held");
        }
    }

    // ---- R-S6b: the tool closure called WITHOUT admission refuses, even with the grant held --------------------
    // The positive control is S5 above: the same tool and grant, run through admission, spawns.
    {
        NativeShellProvider provider({"cmd"}, mount_root, "workdir");
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"cmd", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}}));
        SessionContext sc{"s6b", principal, empty_history};
        auto contribution = on_context_of(provider, sc, ctx);
        if (contribution.has_value() && contribution->tools.size() == 1) {
            auto args_json = json::Value::make_object(
                {{"program", json::Value::make_string("cmd")}, {"args", json::Value::make_array({})}});
            auto reply_json = test_support::run_task_sync<result<json::Value>>(contribution->tools[0].invoke(args_json, ctx));
            AE_CHECK(!reply_json.has_value() && reply_json.error().code == "tool.requirement_unbound",
                      "R-S6b: calling the tool's closure with no admission refuses (tool.requirement_unbound) "
                      "although the grant is held");
        }
    }

    // ---- R-S6c: a program outside the provider's OWNED patterns is refused at the declared bound ------------------
    // The provider owns {"cmd"}; the host also holds a grant covering "powershell". The call is refused by the bound
    // (the provider never owned that program), not by a missing grant -- the control is S5, where "cmd" runs.
    {
        NativeShellProvider provider({"cmd"}, mount_root, "workdir");
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"cmd", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}},
             Capability{cap::NativeExec{"powershell", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}}));
        SessionContext sc{"s6c", principal, empty_history};
        auto contribution = on_context_of(provider, sc, ctx);
        if (contribution.has_value() && contribution->tools.size() == 1) {
            auto args_json = json::Value::make_object(
                {{"program", json::Value::make_string("powershell")}, {"args", json::Value::make_array({})}});
            auto reply_json = invoke_admitted(contribution->tools[0], args_json, ctx);
            AE_CHECK(!reply_json.has_value() && reply_json.error().code == "tool.requirement_out_of_bounds",
                      "R-S6c: a program the provider does not own is refused at the bound although a held grant covers it");
        }
    }

    // ---- R-S7: worktree escape via argv is rejected end-to-end through the tool ---------------
    {
        NativeShellProvider provider({"cmd"}, mount_root, "workdir");
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"cmd", "workdir", cap::Limit::unbounded(), cap::Limit::unbounded(), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}}));
        SessionContext sc{"s7", principal, empty_history};
        auto contribution = on_context_of(provider, sc, ctx);
        AE_CHECK(contribution.has_value() && contribution->tools.size() == 1, "R-S7 setup");
        if (contribution.has_value() && contribution->tools.size() == 1) {
            auto args_json = json::Value::make_object(
                {{"program", json::Value::make_string("cmd")},
                 {"args", json::Value::make_array({json::Value::make_string("/c"),
                                                    json::Value::make_string("type"),
                                                    json::Value::make_string("../../escape.txt")})}});
            auto reply_json = invoke_admitted(contribution->tools[0], args_json, ctx);
            AE_CHECK(!reply_json.has_value(),
                      "R-S7: a '..'-escaping argv entry is rejected before ever reaching the spawned "
                      "child, end to end through the real tool closure");
        }
    }

    // ---- R-S8: the DISCLOSED residual (ADR-071 §6 item 3, GitHub issue #144) ------------------
    // validate_argv_path() checks only arguments that contain a path separator. A shell script passed
    // as ONE argument with none in it (`cd .. && type <file>`) passes unchanged, and the shell walks
    // out of the worktree on its own. This test PINS that residual, so the tool descriptions'
    // "starts in, not confined to" wording stays true: if a later change closes the gap, this check
    // fails and the descriptions and ADR-071 §6 item 3 must be updated with it.
    //
    // The secret sits in the worktree's PARENT, never inside it. Control C1 proves that: the same
    // `type` run without `cd ..` finds nothing. Control C2 proves the separator-bearing form of the
    // same read is still rejected, so R-S8 passes because of the script-string shape, not because
    // validation is off.
    {
        std::filesystem::path const outside_name = "ae_native_providers_test_outside_144.txt";
        std::filesystem::path const outside_file = worktree_path.parent_path() / outside_name;
        std::string const marker = "MARKER-144-outside-the-worktree";
        { std::ofstream(outside_file) << marker << "\n"; }

        NativeShellProvider provider({"cmd"}, mount_root, "workdir");
        EffectContext ctx{.deadline = agentengine::Deadline::none()};
        ctx.capabilities = std::make_shared<CapabilitySet>(CapabilitySet::grant_root(
            {Capability{cap::NativeExec{"cmd", "workdir", cap::Limit::at_most(10000), cap::Limit::at_most(10000), cap::Limit::unbounded(), false, cap::Limit::unbounded(), cap::Limit::unbounded()}}}));
        SessionContext sc{"s8", principal, empty_history};
        auto contribution = on_context_of(provider, sc, ctx);
        AE_CHECK(contribution.has_value() && contribution->tools.size() == 1, "R-S8 setup");
        if (contribution.has_value() && contribution->tools.size() == 1) {
            auto run = [&](std::vector<std::string> const& argv) {
                std::vector<json::Value> items;
                for (auto const& a : argv) items.push_back(json::Value::make_string(a));
                return invoke_admitted(
                    contribution->tools[0],
                    json::Value::make_object({{"program", json::Value::make_string("cmd")},
                                              {"args", json::Value::make_array(std::move(items))}}),
                    ctx);
            };
            auto stdout_of = [](result<json::Value> const& r) -> std::string {
                if (!r.has_value()) return {};
                auto reply = schema::from_json<NativeProcessRunReply>(*r);
                return reply.has_value() ? reply->stdout_text : std::string{};
            };

            auto escaped = run({"/c", "cd .. && type " + outside_name.string()});
            AE_CHECK(escaped.has_value(),
                     "R-S8: a separator-free script string passes argv validation and runs");
            AE_CHECK(stdout_of(escaped).find(marker) != std::string::npos,
                     "R-S8: ... and the shell reads a file OUTSIDE the worktree (the disclosed residual)");

            auto c1 = run({"/c", "type " + outside_name.string()});
            AE_CHECK(stdout_of(c1).find(marker) == std::string::npos,
                     "R-S8 C1: without `cd ..` the same read finds nothing -- the file is not in the worktree");

            auto c2 = run({"/c", "type", "../" + outside_name.string()});
            AE_CHECK(!c2.has_value(),
                     "R-S8 C2: the separator-bearing form of the same read is still rejected");
        }
        std::filesystem::remove(outside_file, ec);
    }

    _putenv_s("PATH", saved_path.c_str());
    std::filesystem::remove_all(scratch_path, ec);
    std::filesystem::remove_all(worktree_path, ec);

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All native_providers (ADR-071) checks passed.\n";
    return 0;
}
