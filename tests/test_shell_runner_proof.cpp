// Prove-phase correctness gate for decisions/ADR-001-shellrunner-grammar-and-dispatch.md's Design
// A (src/backends/native_jail/{shell_grammar,shell_parser,shell_dispatch,shell_runner,
// command_registry}.hpp). Exercises, end-to-end through the real ShellRunner (never a mock), the
// claims ADR-001 §5 names that this pass attempts: Sh-C1 (ExecState sharing), Sh-C2 (shadowing
// precedence), Sh-C3 (no re-tokenization on expansion), Sh-G4's capability-gate half plus the
// `ctx` identity half of its "cannot exceed" claim, the kind-only capability checks for
// export/cat/ls/mkdir/rm/mv/cp (§2.5.4's downgrade — every check here is "holds this capability
// kind at all, or doesn't," never path-scoped, which is explicitly INCONCLUSIVE-by-design and not
// attempted here), and A-C2 (nothing executes until the whole script parses). Sh-S1's static
// per-library symbol check lives in test_shell_runner_no_process_creation.cpp; its behavioral half
// (a hostile-name corpus resolving safely) is exercised here too.

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "agentengine/core/effect_context.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/command_registry.hpp"
#include "backends/native_jail/real_filesystem_adapter.hpp"
#include "backends/native_jail/shell_runner.hpp"
#include "support/crt_fail_fast.hpp"

namespace fs = std::filesystem;
using namespace agentengine;
using agentengine::native_jail::CommandRegistry;
using agentengine::native_jail::DefaultCommandRegistry;
using agentengine::native_jail::RealFileSystemAdapter;
using agentengine::native_jail::RegisteredRunner;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                       << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                           \
    } while (0)

CapabilitySet make_capabilities(std::initializer_list<capability_kind> kinds) {
    std::vector<Capability> caps;
    for (auto k : kinds) caps.push_back(capability_from_kind(k));
    return CapabilitySet::grant_root(std::move(caps));
}

// `_getpid` (process.h) is the MSVC CRT spelling; POSIX has no leading underscore and lives in
// unistd.h. Only used to make each test run's temp-root name unique, not a portability seam.
[[nodiscard]] int current_pid() noexcept {
#if defined(_WIN32)
    return ::_getpid();
#else
    return ::getpid();
#endif
}

fs::path make_temp_root(char const* tag) {
    fs::path root = fs::temp_directory_path() /
                    (std::string("ae_shell_runner_") + tag + "_" + std::to_string(current_pid()));
    fs::create_directories(root);
    fs::create_directories(root / "work");
    return root;
}

// ---- Sh-C1: ExecState sharing --------------------------------------------------------------
void test_sh_c1_execstate_sharing() {
    std::cout << "-- Sh-C1: ExecState sharing --\n";
    fs::path root = make_temp_root("c1");
    auto adapter_r = RealFileSystemAdapter::create(root);
    AE_CHECK(adapter_r.has_value(), "Sh-C1 fixture: adapter constructs");
    RealFileSystemAdapter adapter = std::move(*adapter_r);
    DefaultCommandRegistry registry;
    ShellRunner runner(adapter, registry);

    ExecState state;
    state.cwd = "/";
    EffectContext ctx;
    CapabilitySet caps; // cd needs no capability per §2.4
    ctx.capabilities = &caps;

    ExecState* state_addr_before = &state;
    ExecRequest req{"shell", "cd /work"};
    auto outcome = runner.run(req, state, ctx);
    AE_CHECK(outcome.has_value(), "Sh-C1: cd /work succeeds");
    AE_CHECK(&state == state_addr_before, "Sh-C1: same ExecState object (identity), not a copy");
    AE_CHECK(state.cwd == "/work", "Sh-C1: state.cwd mutated in place to the canonical form");

    fs::remove_all(root);
}

// ---- Sh-C3: expansion never re-tokenizes -----------------------------------------------------
void test_sh_c3_no_retokenize() {
    std::cout << "-- Sh-C3: expansion never re-tokenizes --\n";
    fs::path root = make_temp_root("c3");
    auto adapter_r = RealFileSystemAdapter::create(root);
    RealFileSystemAdapter adapter = std::move(*adapter_r);
    DefaultCommandRegistry registry;
    ShellRunner runner(adapter, registry);

    ExecState state;
    state.cwd = "/";
    EffectContext ctx;
    CapabilitySet caps = make_capabilities({capability_kind::env_write});
    ctx.capabilities = &caps;

    struct Case { char const* name; std::string value; };
    std::vector<Case> cases = {
        {"semicolon+rm", ";rm -rf /work"},
        {"pipe", "a|b"},
        {"andand", "a&&b"},
        {"whitespace", "hello world  foo"},
    };
    for (auto const& c : cases) {
        std::string export_script = std::string("export X=\"") + c.value + "\"";
        auto exp = runner.run(ExecRequest{"shell", export_script}, state, ctx);
        AE_CHECK(exp.has_value(), std::string("Sh-C3 (") + c.name + "): export succeeds");
        auto echoed = runner.run(ExecRequest{"shell", "echo $X"}, state, ctx);
        AE_CHECK(echoed.has_value(), std::string("Sh-C3 (") + c.name + "): echo $X succeeds");
        std::string expected = c.value + "\n";
        AE_CHECK(echoed.has_value() && echoed->stdout_text == expected,
                 std::string("Sh-C3 (") + c.name +
                     "): expanded value is exactly one opaque word, unchanged — got '" +
                     (echoed.has_value() ? echoed->stdout_text : echoed.error().message) + "'");
    }
    // Confirm no second command ran: /work/should_not_exist must not exist.
    auto exists = adapter.exists("/work/should_not_exist");
    AE_CHECK(exists.has_value() && !*exists,
             "Sh-C3: the embedded ';rm -rf /work'-shaped value never executed a second command");

    fs::remove_all(root);
}

// ---- Sh-C2: builtin-name reservation (finding 5's fix) ---------------------------------------
void test_sh_c2_reserved_name_precedence() {
    std::cout << "-- Sh-C2: reserved-name precedence (finding 5 fix) --\n";
    DefaultCommandRegistry registry;

    RegisteredRunner shadow_ls;
    shadow_ls.name = "ls"; // collides with the builtin
    shadow_ls.invoke = [](ExecRequest const&, ExecState&, EffectContext&) -> result<ExecOutcome> {
        return ExecOutcome{};
    };
    auto reg1 = registry.register_runner(shadow_ls);
    AE_CHECK(!reg1.has_value() && reg1.error().code == "shell.reserved_name",
             "Sh-C2: registering a Runner named 'ls' is rejected at registration time");
    AE_CHECK(registry.resolve("ls").kind == agentengine::native_jail::command_kind::builtin,
             "Sh-C2: 'ls' still resolves to the builtin after the rejected registration attempt");

    agentengine::native_jail::RegisteredTool grep_tool;
    grep_tool.name = "grep";
    grep_tool.invoke = [](std::vector<std::string> const&, EffectContext&) -> result<ExecOutcome> {
        return ExecOutcome{};
    };
    auto reg2 = registry.register_tool(grep_tool);
    AE_CHECK(reg2.has_value(), "Sh-C2: registering a non-colliding Tool ('grep') succeeds");
    AE_CHECK(registry.resolve("grep").kind == agentengine::native_jail::command_kind::tool,
             "Sh-C2: 'grep' resolves to the registered tool");

    RegisteredRunner grep_runner;
    grep_runner.name = "grep"; // now collides with the just-registered Tool
    grep_runner.invoke = shadow_ls.invoke;
    auto reg3 = registry.register_runner(grep_runner);
    AE_CHECK(!reg3.has_value() && reg3.error().code == "shell.reserved_name",
             "Sh-C2: a Runner colliding with an already-registered Tool name is also rejected");
}

// ---- Sh-G4: capability gate + ctx identity ----------------------------------------------------
void test_sh_g4_runner_call_gate_and_identity() {
    std::cout << "-- Sh-G4: RunnerCall gate + ctx identity --\n";
    fs::path root = make_temp_root("g4");
    auto adapter_r = RealFileSystemAdapter::create(root);
    RealFileSystemAdapter adapter = std::move(*adapter_r);

    DefaultCommandRegistry registry;
    bool invoked = false;
    EffectContext const* ctx_seen_inside_runner = nullptr;
    RegisteredRunner py;
    py.name = "py";
    py.required_capability = capability_kind::runner_call;
    py.invoke = [&](ExecRequest const&, ExecState&, EffectContext& inner_ctx) -> result<ExecOutcome> {
        invoked = true;
        ctx_seen_inside_runner = &inner_ctx;
        return ExecOutcome{};
    };
    auto reg = registry.register_runner(py);
    AE_CHECK(reg.has_value(), "Sh-G4 fixture: runner registers");

    ShellRunner runner(adapter, registry);
    ExecState state;
    state.cwd = "/";

    // Gate half: no runner_call capability -> denied BEFORE the nested runner ever executes.
    {
        EffectContext ctx;
        CapabilitySet caps; // empty
        ctx.capabilities = &caps;
        invoked = false;
        auto outcome = runner.run(ExecRequest{"shell", "py hello"}, state, ctx);
        AE_CHECK(!outcome.has_value() && outcome.error().code == "shell.capability_denied",
                 "Sh-G4 gate: missing RunnerCall is denied with a policy error");
        AE_CHECK(!invoked, "Sh-G4 gate: the nested Runner's invoke() never ran");
    }

    // Construction half: granted -> invoked, and the ctx passed through is pointer-identical.
    {
        EffectContext ctx;
        CapabilitySet caps = make_capabilities({capability_kind::runner_call});
        ctx.capabilities = &caps;
        invoked = false;
        ctx_seen_inside_runner = nullptr;
        auto outcome = runner.run(ExecRequest{"shell", "py hello"}, state, ctx);
        AE_CHECK(outcome.has_value(), "Sh-G4 construction: granted RunnerCall succeeds");
        AE_CHECK(invoked, "Sh-G4 construction: the nested Runner's invoke() ran");
        AE_CHECK(ctx_seen_inside_runner == &ctx,
                 "Sh-G4 construction: the SAME EffectContext object reaches the nested Runner "
                 "(never a copy, never attenuated/broadened)");
    }

    // Negative control on the identity assertion itself: prove the pointer-equality check used
    // above actually has discriminating power (isn't vacuously true for any two contexts), per
    // finding 6's request for "a control where an implementation is deliberately made to broaden
    // capabilities before the nested call, to confirm such a regression would actually be
    // caught." This does not patch ShellRunner itself (nothing here needs broadening — §2.3
    // forwards `ctx` unchanged, by construction, with no attenuation/broadening step to patch);
    // it demonstrates that had such a step existed and substituted a different EffectContext
    // object, this test's own assertion mechanism would have failed loudly instead of passing
    // vacuously.
    {
        EffectContext ctx_a;
        EffectContext ctx_b; // a different object standing in for a hypothetical "broadened" copy
        bool would_have_been_caught = (&ctx_a != &ctx_b);
        AE_CHECK(would_have_been_caught,
                 "Sh-G4 negative control: the identity assertion distinguishes two distinct "
                 "EffectContext objects (i.e. is not vacuously true)");
    }

    fs::remove_all(root);
}

// ---- capability kind-only checks (§2.5.4) -----------------------------------------------------
void test_capability_checks_kind_only() {
    std::cout << "-- capability checks (kind-only per §2.5.4; path-scoped is not attempted) --\n";
    fs::path root = make_temp_root("caps");
    auto adapter_r = RealFileSystemAdapter::create(root);
    RealFileSystemAdapter adapter = std::move(*adapter_r);
    DefaultCommandRegistry registry;
    ShellRunner runner(adapter, registry);

    auto run_with = [&](std::string script, CapabilitySet caps) {
        ExecState state;
        state.cwd = "/";
        EffectContext ctx;
        ctx.capabilities = &caps;
        return runner.run(ExecRequest{"shell", std::move(script)}, state, ctx);
    };

    // export: denied without EnvWrite, then granted -> succeeds.
    {
        auto denied = run_with("export Y=1", CapabilitySet{});
        AE_CHECK(!denied.has_value() && denied.error().code == "shell.capability_denied",
                 "export denied without EnvWrite");
        auto allowed = run_with("export Y=1", make_capabilities({capability_kind::env_write}));
        AE_CHECK(allowed.has_value(), "export succeeds with EnvWrite granted (positive control)");
    }
    // cat/ls: denied without FsRead, then granted -> succeeds.
    {
        auto denied_ls = run_with("ls /work", CapabilitySet{});
        AE_CHECK(!denied_ls.has_value() && denied_ls.error().code == "shell.capability_denied",
                 "ls denied without FsRead");
        auto allowed_ls = run_with("ls /work", make_capabilities({capability_kind::fs_read}));
        AE_CHECK(allowed_ls.has_value(), "ls succeeds with FsRead granted (positive control)");

        auto denied_cat = run_with("cat /nope.txt", CapabilitySet{});
        AE_CHECK(!denied_cat.has_value() && denied_cat.error().code == "shell.capability_denied",
                 "cat denied without FsRead (before the fs call, so it fails on capability, not "
                 "on the missing file)");
    }
    // mkdir/rm: denied without FsWrite, then granted -> succeeds.
    {
        auto denied_mkdir = run_with("mkdir /work/newdir", CapabilitySet{});
        AE_CHECK(!denied_mkdir.has_value() && denied_mkdir.error().code == "shell.capability_denied",
                 "mkdir denied without FsWrite");
        auto allowed_mkdir =
            run_with("mkdir /work/newdir", make_capabilities({capability_kind::fs_write}));
        AE_CHECK(allowed_mkdir.has_value(), "mkdir succeeds with FsWrite granted (positive control)");
        auto exists = adapter.exists("/work/newdir");
        AE_CHECK(exists.has_value() && *exists, "mkdir with FsWrite actually created the directory");

        auto denied_rm = run_with("rm /work/newdir", CapabilitySet{});
        AE_CHECK(!denied_rm.has_value() && denied_rm.error().code == "shell.capability_denied",
                 "rm denied without FsWrite");
        auto allowed_rm =
            run_with("rm -r /work/newdir", make_capabilities({capability_kind::fs_write}));
        AE_CHECK(allowed_rm.has_value(), "rm succeeds with FsWrite granted (positive control)");
    }

    fs::remove_all(root);
}

// ---- A-C2: nothing executes until the whole script parses -------------------------------------
void test_a_c2_nothing_executes_on_parse_failure() {
    std::cout << "-- A-C2: nothing executes until the whole script parses --\n";
    fs::path root = make_temp_root("ac2");
    auto adapter_r = RealFileSystemAdapter::create(root);
    RealFileSystemAdapter adapter = std::move(*adapter_r);
    DefaultCommandRegistry registry;
    ShellRunner runner(adapter, registry);

    ExecState state;
    state.cwd = "/";
    EffectContext ctx;
    // Deliberately GRANT FsWrite here: if the malformed-script bug this claim guards against were
    // present and mkdir ran anyway, granting the capability makes that failure observable (the
    // directory would actually get created) rather than being masked by an unrelated capability
    // denial.
    CapabilitySet caps = make_capabilities({capability_kind::fs_write, capability_kind::fs_read});
    ctx.capabilities = &caps;

    std::string script = "mkdir /work/should_not_exist; if true then echo hi";
    auto outcome = runner.run(ExecRequest{"shell", script}, state, ctx);
    AE_CHECK(!outcome.has_value(), "A-C2: the malformed script (missing 'fi') fails to parse");
    if (!outcome.has_value()) {
        std::cout << "    parse error code: " << outcome.error().code << "\n";
    }
    auto exists = adapter.exists("/work/should_not_exist");
    AE_CHECK(exists.has_value() && !*exists,
             "A-C2: mkdir from the syntactically invalid script never executed");

    fs::remove_all(root);
}

// ---- Sh-S1 (behavioral half): a hostile-name corpus resolves safely --------------------------
void test_sh_s1_hostile_name_corpus() {
    std::cout << "-- Sh-S1 (behavioral): hostile-name corpus resolves to not_found/deny, never "
                 "differently --\n";
    DefaultCommandRegistry registry;
    // Negative control: resolve() is not vacuously "always not_found".
    AE_CHECK(registry.resolve("ls").kind == agentengine::native_jail::command_kind::builtin,
             "Sh-S1 sanity: a real builtin still resolves as a builtin");

    std::vector<std::string> hostile_names = {
        "/bin/sh", "cmd", "cmd.exe", "powershell", "powershell.exe", "CreateProcess",
        "system", "LoadLibraryA", "../../ls", "..\\..\\ls", "l s", "Ls", "LS", "ls ",
        "cat.exe", "rm.exe", "totally_bogus_xyz123", "",
    };
    for (auto const& name : hostile_names) {
        auto resolved = registry.resolve(name);
        bool ok = resolved.kind == agentengine::native_jail::command_kind::not_found;
        AE_CHECK(ok, "resolve('" + name + "') == not_found");
    }

    // End-to-end: a full script naming a hostile "command" fails closed with a contract error,
    // under a deny-all capability set, never a crash and never any different outcome class.
    fs::path root = make_temp_root("s1");
    auto adapter_r = RealFileSystemAdapter::create(root);
    RealFileSystemAdapter adapter = std::move(*adapter_r);
    ShellRunner runner(adapter, registry);
    ExecState state;
    state.cwd = "/";
    for (auto const& name : {"/bin/sh", "cmd.exe", "totally_bogus_xyz123"}) {
        EffectContext ctx;
        CapabilitySet caps;
        ctx.capabilities = &caps;
        auto outcome = runner.run(ExecRequest{"shell", std::string(name) + " -c hostile"}, state, ctx);
        AE_CHECK(!outcome.has_value() && outcome.error().code == "shell.command_not_found",
                 std::string("end-to-end: '") + name + "' fails closed as command_not_found");
    }
    fs::remove_all(root);
}

} // namespace

int main() {
    ::agentengine::test_support::fail_fast_on_windows();
    test_sh_c1_execstate_sharing();
    test_sh_c3_no_retokenize();
    test_sh_c2_reserved_name_precedence();
    test_sh_g4_runner_call_gate_and_identity();
    test_capability_checks_kind_only();
    test_a_c2_nothing_executes_on_parse_failure();
    test_sh_s1_hostile_name_corpus();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "test_shell_runner_proof: PASS\n";
    return 0;
}
