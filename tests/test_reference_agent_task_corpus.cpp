// Milestone 3 Phase H1 (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, 026 §8
// G1 -- "under native-jail, a scripted data task ... produces ... an artifact identically across the
// target platform set ... from the same pinned preinstalled image"). This is the model-DEPENDENT half
// of Phase H1 (the model-independent half, 026 §7's prompt budget and §8 G3's no-leakage rule, is
// tests/test_reference_agent_prompt.cpp).
//
// Three tasks, each backed by a HAND-AUTHORED fixture under
// tests/fixtures/chat_client/reference_agent/ (tests/support/recorded_chat_client.hpp's own header
// comment is explicit: its fixtures are "authored by a human, not captured from a live call" -- 004
// §6's general record/replay feature does not exist, and this phase does not build a one-off
// exception to that just because a real OpenRouter-compatible credential happened to be available
// locally this session; see api-test.txt, gitignored, deliberately unused here). Each fixture plays
// back a plausible reference-agent response (narration + a fenced ```python block, the same shape a
// real CodeAct-style model reply takes) via `RecordedChatClient`, wholly independent of the
// `ChatRequest` passed in (RecordedChatClient's own documented contract). This test extracts the code
// from that fixed response and actually executes it through the real `MediatedPythonRunner` under a
// real capability grant -- proving the HARNESS correctly measures first-attempt execution success for
// a recorded instance, not that a live model is prompt-invariant in general (that would need many
// live samples across many models, out of scope for a spec-driven regression test).
//
// Tasks (as scoped and agreed for this phase -- matplotlib is NOT installed in this environment, so
// the third task produces a real, non-chart artifact instead; a rendered chart is a named residual):
//   1. csv_sum        -- read a CSV, sum a numeric column (plain csv module, plain open()).
//   2. list_dir        -- list files under a mount path, sorted (agent.files.list -- raw os.listdir
//                          on a virtual /mount/path is NOT mediated, only open()/listdir() are, so a
//                          plausible model reply reaches for the documented agent.files convenience).
//   3. pandas_groupby  -- pandas groupby producing a CSV summary artifact via agent.files.artifact
//                          (pandas.DataFrame.to_csv(path) does its own unmediated os.path.isdir
//                          pre-check against the virtual /out path and fails before ever calling
//                          open() -- a real, load-bearing finding from building this task, not a
//                          hypothetical; agent.files.artifact's write already goes through the same
//                          mediated open() path proven in Phase G2, so this sidesteps it rather than
//                          papering over it).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"
#include "support/recorded_chat_client.hpp"
#include "support/run_task_sync.hpp"

#ifndef AE_TEST_FIXTURE_DIR
#error "AE_TEST_FIXTURE_DIR must be defined by CMake to tests/fixtures/chat_client/reference_agent"
#endif

using namespace agentengine;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;
using agentengine::test_support::RecordedChatClient;
using agentengine::test_support::run_task_sync;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::fprintf(stderr, "FAIL: %s at %s:%d\n", (label), __FILE__, __LINE__);              \
            ++g_failures;                                                                          \
        } else {                                                                                   \
            std::printf("  ok: %s\n", (label));                                                    \
        }                                                                                           \
    } while (0)

void write_file(std::filesystem::path const& path, std::string const& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

std::string read_file(std::filesystem::path const& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Extracts the first fenced ```python ... ``` block from a reference agent's text reply -- the same
// narration+code-fence shape a real CodeAct-style response takes. A harness convention specific to
// this test, not production parsing (there is no fixed content-item "kind" for extracted code; a real
// harness would define one, out of scope for this phase).
std::string extract_python_code(std::string const& text) {
    std::string const open_fence = "```python";
    auto const start = text.find(open_fence);
    if (start == std::string::npos) return {};
    auto const code_start = start + open_fence.size();
    auto const end = text.find("```", code_start);
    if (end == std::string::npos) return {};
    std::string code = text.substr(code_start, end - code_start);
    if (!code.empty() && code.front() == '\n') code.erase(code.begin());
    return code;
}

}  // namespace

int main() {
    std::string const base = ::agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") +
                              "/ae_h1_task_corpus";
    std::filesystem::path input_dir = std::filesystem::path(base) / "input";
    std::filesystem::path work_dir = std::filesystem::path(base) / "work";
    std::filesystem::path out_dir = std::filesystem::path(base) / "out";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(input_dir);
    std::filesystem::create_directories(work_dir);
    std::filesystem::create_directories(out_dir);

    write_file(input_dir / "sales.csv", "date,category,amount\n2026-01-01,widgets,12.50\n2026-01-02,widgets,7.25\n"
                                         "2026-01-03,gadgets,30.00\n");
    std::filesystem::create_directories(input_dir / "listing_demo");
    write_file(input_dir / "listing_demo" / "alpha.txt", "a");
    write_file(input_dir / "listing_demo" / "beta.txt", "b");
    write_file(input_dir / "listing_demo" / "gamma.txt", "c");
    write_file(input_dir / "orders.csv", "category,amount\nwidgets,10.00\nwidgets,5.00\ngadgets,20.00\n"
                                          "gadgets,4.50\n");

    auto widen = [](std::filesystem::path const& p) { return p.wstring(); };

    // Measured top-level transitive-import closure of `import numpy, pandas` (ADR-002 §8, also used
    // verbatim by tests/test_python_numpy_pandas_import.cpp) -- reused here rather than re-measured,
    // since it is a superset that already covers the plain `csv`/`os` tasks too. A session-wide
    // package policy (010 §5) would eventually populate this; until then this IS the fixed, curated
    // set the whole task corpus runs under, matching "one image, one closure" rather than a
    // per-task-tailored allowlist a real reference agent's session would never have.
    std::unordered_set<std::string> const closure = {
        "_imp", "__future__", "_ast", "_bisect", "_blake2", "_bz2", "_collections", "_collections_abc",
        "_compat_pickle", "_compression", "_contextvars", "_csv", "_ctypes", "_cython_3_1_3",
        "_cython_3_1_4", "_datetime", "_decimal", "_functools", "_hashlib", "_json", "_locale",
        "_lzma", "_opcode", "_opcode_metadata", "_operator", "_pickle", "_random", "_sre", "_stat",
        "_string", "_strptime", "_struct", "_sysconfig", "_tokenize", "_typing", "_uuid",
        "_weakrefset", "_winapi", "_wmi", "_zoneinfo", "ast", "base64", "binascii", "bisect",
        "bz2", "calendar", "cmath", "collections", "contextlib", "contextvars", "copy", "copyreg",
        "csv", "ctypes", "cython_runtime", "dataclasses", "datetime", "dateutil", "decimal", "dis",
        "enum", "errno", "fnmatch", "functools", "gc", "genericpath", "glob", "gzip", "hashlib",
        "hmac", "importlib", "inspect", "io", "ipaddress", "itertools", "json", "keyword",
        "linecache", "locale", "lzma", "math", "mkl", "mmap", "msvcrt", "nt", "ntpath", "numbers",
        "numpy", "opcode", "operator", "os", "pandas", "pathlib", "pickle", "platform",
        "posixpath", "pprint", "pytz", "random", "re", "reprlib", "secrets", "shutil", "signal",
        "six", "stat", "string", "struct", "subprocess", "sysconfig", "tarfile", "tempfile",
        "textwrap", "threading", "time", "token", "tokenize", "types", "typing", "unicodedata",
        "urllib", "uuid", "warnings", "weakref", "winreg", "zipfile", "zlib", "zoneinfo",
    };

    MediatedPythonConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    cfg.extra_sys_path = {AE_PYTHON_SITE_PACKAGES};
    cfg.package_policy_allowlist = closure;
    cfg.mount_roots["input"] = widen(input_dir);
    cfg.mount_roots["work"] = widen(work_dir);
    cfg.mount_roots["out"] = widen(out_dir);
    cfg.expose_agent_files_data = true;

    MediatedPythonRunner runner(std::move(cfg));
    auto init = runner.initialize();
    AE_CHECK(init.has_value(), "H1-setup: MediatedPythonRunner initializes with the measured closure");

    auto caps = CapabilitySet::grant_root({
        Capability{cap::FsRead{"input", "", std::nullopt}},
        Capability{cap::FsRead{"work", "", std::nullopt}},
        Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
        Capability{cap::FsRead{"out", "", std::nullopt}},
        Capability{cap::FsWrite{"out", "", std::nullopt, std::nullopt}},
    });

    // ---- Task 1: csv_sum ---------------------------------------------------------------------
    {
        RecordedChatClient client(std::filesystem::path(AE_TEST_FIXTURE_DIR) / "csv_sum.json");
        EffectContext dummy_ctx{};
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(ChatRequest{}, dummy_ctx));
        AE_CHECK(resp.has_value(), "H1-T1: fixture loads and plays back a response");

        std::string code;
        if (resp.has_value() && !resp->message.content.empty()) {
            if (auto const* text = std::get_if<Text>(&resp->message.content.front().value)) {
                code = extract_python_code(text->text);
            }
        }
        AE_CHECK(!code.empty(), "H1-T1: a python code block is extracted from the fixture response");

        ExecState state{};
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);
        ExecRequest req{"python", code};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                 "H1-T1: the extracted code executes successfully first-attempt, under the real sandbox");
        AE_CHECK(out.has_value() && out->stdout_text.find("TOTAL: 49.75") != std::string::npos,
                 "H1-T1: the printed sum matches the real /input/sales.csv content (12.50+7.25+30.00)");
    }

    // ---- Task 2: list_dir ---------------------------------------------------------------------
    {
        RecordedChatClient client(std::filesystem::path(AE_TEST_FIXTURE_DIR) / "list_dir.json");
        EffectContext dummy_ctx{};
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(ChatRequest{}, dummy_ctx));
        AE_CHECK(resp.has_value(), "H1-T2: fixture loads and plays back a response");

        std::string code;
        if (resp.has_value() && !resp->message.content.empty()) {
            if (auto const* text = std::get_if<Text>(&resp->message.content.front().value)) {
                code = extract_python_code(text->text);
            }
        }
        AE_CHECK(!code.empty(), "H1-T2: a python code block is extracted from the fixture response");

        ExecState state{};
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);
        ExecRequest req{"python", code};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                 "H1-T2: the extracted code executes successfully first-attempt, under the real sandbox");
        AE_CHECK(out.has_value() &&
                     out->stdout_text.find("alpha.txt\nbeta.txt\ngamma.txt") != std::string::npos,
                 "H1-T2: the printed listing matches the real /input/listing_demo directory, sorted");
    }

    // ---- Task 3: pandas_groupby -----------------------------------------------------------------
    {
        RecordedChatClient client(std::filesystem::path(AE_TEST_FIXTURE_DIR) / "pandas_groupby.json");
        EffectContext dummy_ctx{};
        auto resp = run_task_sync<result<ChatResponse>>(client.chat(ChatRequest{}, dummy_ctx));
        AE_CHECK(resp.has_value(), "H1-T3: fixture loads and plays back a response");

        std::string code;
        if (resp.has_value() && !resp->message.content.empty()) {
            if (auto const* text = std::get_if<Text>(&resp->message.content.front().value)) {
                code = extract_python_code(text->text);
            }
        }
        AE_CHECK(!code.empty(), "H1-T3: a python code block is extracted from the fixture response");

        ExecState state{};
        EffectContext ctx{};
        ctx.capabilities = agentengine::borrow_capabilities(caps);
        ExecRequest req{"python", code};
        auto out = runner.run(req, state, ctx);
        AE_CHECK(out.has_value() && out->klass == exec_outcome_class::ok,
                 "H1-T3: the extracted code executes successfully first-attempt, under the real sandbox");
        AE_CHECK(out.has_value() && out->stdout_text.find("WROTE: 2 rows") != std::string::npos,
                 "H1-T3: pandas groupby over the real /input/orders.csv produces 2 category rows");

        std::string const artifact = read_file(out_dir / "summary.csv");
        AE_CHECK(artifact.find("gadgets,24.5") != std::string::npos,
                 "H1-T3: the real artifact on disk contains the correct gadgets total (20.00+4.50)");
        AE_CHECK(artifact.find("widgets,15.0") != std::string::npos,
                 "H1-T3: the real artifact on disk contains the correct widgets total (10.00+5.00) -- "
                 "a real, non-chart artifact (026 §9/010 §9 G1); a rendered matplotlib chart is a "
                 "named residual, since matplotlib is not installed in this environment");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All reference-agent task-corpus checks passed.\n");
    return 0;
}
