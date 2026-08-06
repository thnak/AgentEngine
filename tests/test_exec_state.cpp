// Proof for 010-Python-Code-Interpreter.md §3a's `ExecState` ("one instance per session, held by
// the sandbox and passed by reference into whichever Runner executes"), Milestone 3 Phase E1
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md). `PythonRunner`/
// `ShellRunner` do not exist yet (Phase E2/E3) -- this proves the part of §3a that does not need
// them: `SessionExecStateRegistry` (sandbox/runner.hpp) hands back the SAME `ExecState&` for a
// given session on every call, distinct sessions never share one, and destroying a session's
// `ExecState` is a real teardown, not a hidden entry.
//
// The two mock Runners below are deliberately real `Runner`-concept-conforming types (proven by
// `static_assert`, 022 §5), not strawmen: this is what lets E1-C3/C4 below reproduce §3a's own
// worked examples verbatim ("a `cd` in a shell command mutates the same ExecState a subsequent
// execute_code call reads", "Python's own `os` calls and ShellRunner observe the same ExecState")
// without needing the real interpreter/shell to exist first.

#include <iostream>
#include <string>

#include "agentengine/sandbox/runner.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " at " << __FILE__ << ":" << __LINE__ << "\n";     \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::cout << "  ok: " << (label) << "\n";                                              \
        }                                                                                           \
    } while (0)

// Stands in for `ShellRunner`: a `cd` that mutates the shared `ExecState.cwd`, and sets an env var
// the way `export` would.
struct MockShellRunner {
    result<ExecOutcome> run(ExecRequest request, ExecState& state, EffectContext& ctx) {
        (void)request;
        (void)ctx;
        state.cwd            = "/tmp/from-shell-cd";
        state.env["FROM_SH"] = "1";
        return ExecOutcome{};
    }
};

// Stands in for `PythonRunner`: `os.getcwd()`/`os.environ` reading whatever the shared ExecState
// currently holds, and `os.chdir()` mutating it right back.
struct MockPythonRunner {
    std::string observed_cwd;
    bool        observed_shell_env = false;

    result<ExecOutcome> run(ExecRequest request, ExecState& state, EffectContext& ctx) {
        (void)request;
        (void)ctx;
        observed_cwd       = state.cwd;
        observed_shell_env = state.env.contains("FROM_SH");
        state.cwd          = "/tmp/from-python-chdir";
        state.env["FROM_PY"] = "1";
        return ExecOutcome{};
    }
};

static_assert(Runner<MockShellRunner>, "MockShellRunner must be a real Runner-concept-conforming type");
static_assert(Runner<MockPythonRunner>, "MockPythonRunner must be a real Runner-concept-conforming type");

} // namespace

int main() {
    // E1-C1: a fresh session's ExecState is default-constructed (empty cwd, empty env) on first
    // access -- nothing pre-seeded, nothing carried over from a prior test run's process state.
    {
        SessionExecStateRegistry registry;
        ExecState& state = registry.get_or_create("session:e1-1");
        AE_CHECK(state.cwd.empty() && state.env.empty(),
                 "E1-C1: a fresh session's ExecState starts default-constructed");
    }

    // E1-C2: repeated get_or_create for the SAME session id returns a reference to the IDENTICAL
    // object -- mutate through one reference, observe through a second, independently-obtained one.
    {
        SessionExecStateRegistry registry;
        ExecState& first = registry.get_or_create("session:e1-2");
        first.cwd         = "/mutated-via-first-reference";

        ExecState& second = registry.get_or_create("session:e1-2");
        AE_CHECK(second.cwd == "/mutated-via-first-reference",
                 "E1-C2: a second get_or_create call for the same session sees the first's mutation "
                 "-- the same object, not a copy");
        AE_CHECK(&first == &second,
                 "E1-C2: both references are the identical object's address, not merely equal content");
    }

    // E1-C3 (010 §3a's own worked example, reproduced literally): "a cd in a shell command mutates
    // the same ExecState a subsequent execute_code call reads via os.getcwd()."
    {
        SessionExecStateRegistry registry;
        ExecState& state = registry.get_or_create("session:e1-3");
        EffectContext ctx{};

        MockShellRunner shell;
        auto shell_result = shell.run(ExecRequest{"shell", "cd /tmp/from-shell-cd"}, state, ctx);
        AE_CHECK(shell_result.has_value(), "E1-C3: setup -- the shell mock's cd runs");

        MockPythonRunner python;
        auto python_result = python.run(ExecRequest{"python", "os.getcwd()"}, state, ctx);
        AE_CHECK(python_result.has_value(), "E1-C3: setup -- the python mock's read runs");
        AE_CHECK(python.observed_cwd == "/tmp/from-shell-cd",
                 "E1-C3: the python mock observes the SAME cwd the shell mock just set -- shared by "
                 "reference, not a synchronized pair");
        AE_CHECK(python.observed_shell_env,
                 "E1-C3: the python mock also observes the env var the shell mock exported, through "
                 "the identical ExecState");
    }

    // E1-C4 (§3a's other direction): "os.chdir() in Python mutates the one a subsequent shell
    // command reads via pwd" -- proven by running the mocks in the OPPOSITE order from E1-C3,
    // confirming the sharing property is not accidentally one-directional.
    {
        SessionExecStateRegistry registry;
        ExecState& state = registry.get_or_create("session:e1-4");
        EffectContext ctx{};

        MockPythonRunner python;
        auto python_result = python.run(ExecRequest{"python", "os.chdir('/tmp/from-python-chdir')"}, state, ctx);
        AE_CHECK(python_result.has_value(), "E1-C4: setup -- the python mock's chdir runs");

        AE_CHECK(state.cwd == "/tmp/from-python-chdir",
                 "E1-C4: the shared ExecState reflects python's chdir immediately, before any shell "
                 "call reads it");

        MockShellRunner shell;
        auto shell_result = shell.run(ExecRequest{"shell", "pwd"}, state, ctx);
        AE_CHECK(shell_result.has_value(), "E1-C4: setup -- the shell mock's pwd-equivalent runs");
        // The shell mock always sets its own fixed cwd (it doesn't need to READ cwd to prove this
        // direction) -- what matters is that BEFORE it ran, the state it would have read via `pwd`
        // was already python's chdir target, confirmed above.
    }

    // E1-R1 (isolation, §3a: "two sessions never share an ExecState any more than they share a
    // heap"): two distinct session ids get independent ExecState instances.
    {
        SessionExecStateRegistry registry;
        ExecState& a = registry.get_or_create("session:e1-r1-a");
        ExecState& b = registry.get_or_create("session:e1-r1-b");
        a.cwd = "/only-a";
        b.cwd = "/only-b";
        AE_CHECK(registry.get_or_create("session:e1-r1-a").cwd == "/only-a" &&
                     registry.get_or_create("session:e1-r1-b").cwd == "/only-b",
                 "E1-R1: two sessions' ExecState objects never observe each other's mutation");
    }

    // E1-R2: destroy() is a real teardown, not merely hiding the entry -- a subsequent get_or_create
    // for the SAME id constructs a genuinely FRESH object, never resurrecting the mutated one.
    {
        SessionExecStateRegistry registry;
        ExecState& first = registry.get_or_create("session:e1-r2");
        first.cwd         = "/about-to-be-destroyed";
        AE_CHECK(registry.has("session:e1-r2"), "E1-R2: setup -- the session's ExecState is present");

        registry.destroy("session:e1-r2");
        AE_CHECK(!registry.has("session:e1-r2"), "E1-R2: destroy() removes the session's entry");

        ExecState& recreated = registry.get_or_create("session:e1-r2");
        AE_CHECK(recreated.cwd.empty(),
                 "E1-R2: a fresh get_or_create for the same id after destroy() is genuinely "
                 "default-constructed, not the old mutated object resurrected");
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All ExecState/SessionExecStateRegistry proof checks passed.\n";
    return 0;
}
