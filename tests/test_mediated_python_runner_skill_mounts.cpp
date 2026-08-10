// Closes the exact gap this session's research identified: every prior skills test proved
// `mount_read` (a real, content-addressed round trip) ONLY from test-harness C++ calling it directly
// -- no test had ever proven the AGENT ITSELF, via real Python code running inside a real embedded
// CPython interpreter, reading a mounted skill's content. This file does: a real `SkillsProvider`
// resolves a real inline skill, `native_jail::materialize_skill_mounts` writes it to a real host
// directory, that directory becomes a real `MediatedPythonConfig::mount_roots` entry, and a real
// `MediatedPythonRunner::run()` executes Python code that does `open('/<name>/SKILL.md').read()` --
// the SAME globally-mediated `open()` every guest code path uses, no special-cased shortcut.
//
// `AGENTENGINE_BUILD_PYTHON_RUNNER` is a CMake configure-time option only (never forwarded as a
// compiler `#define` anywhere in this build -- confirmed while building tools/cli_chat.cpp this same
// session, where a stale `#ifdef AGENTENGINE_BUILD_PYTHON_RUNNER` silently compiled a stub instead of
// the real implementation). This file therefore carries NO internal preprocessor guard on that macro
// -- gating happens once, in tests/CMakeLists.txt, by only registering this target when
// AGENTENGINE_BUILD_PYTHON_RUNNER is ON, matching every other real Python-runner test in this suite
// (e.g. test_mediated_python_runner_agent_files_data.cpp).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "agentengine/core/skill_provider.hpp"
#include "backends/native_jail/mediated_python_runner.hpp"
#include "backends/native_jail/skill_mount_materializer.hpp"

using namespace agentengine;
using agentengine::native_jail::MediatedPythonConfig;
using agentengine::native_jail::MediatedPythonRunner;

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

[[nodiscard]] SkillSourceResult make_skill(std::string name, std::string description,
                                            std::string body_text) {
    std::string const doc = "---\nname: " + name + "\ndescription: " + description + "\n---\n" + body_text;
    auto skill = parse_skill_md(doc, name);
    SkillSourceResult r;
    r.skill = *skill;
    std::vector<std::byte> manifest_bytes(reinterpret_cast<std::byte const*>(doc.data()),
                                           reinterpret_cast<std::byte const*>(doc.data()) + doc.size());
    r.files.push_back(SkillBundleFile{"SKILL.md", std::move(manifest_bytes)});
    return r;
}

}  // namespace

int main() {
    std::string const base = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "C:/Windows/Temp") +
                              "/ae_skill_mounts_python_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);

    std::vector<SkillSourceDescriptor> sources;
    sources.push_back(make_skill_source_descriptor(InlineSkillSource(
        "origin-a",
        {make_skill("real-mounted-skill", "A skill whose content the agent reads for real.",
                    "REAL SKILL BODY -- read via a real mediated open() call.\n")})));
    SkillsProvider<> provider(std::move(sources));

    auto materialized =
        native_jail::materialize_skill_mounts(provider, std::filesystem::path(base) / "skills", {"work"});
    check(materialized.has_value(), "setup: materialize_skill_mounts succeeds for one real skill");
    if (!materialized || materialized->empty()) {
        std::fprintf(stderr, "test_mediated_python_runner_skill_mounts: 1 FAILURE(S) (setup)\n");
        return 1;
    }

    MediatedPythonConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    for (auto const& [mount_id, host_dir] : *materialized) cfg.mount_roots[mount_id] = host_dir;
    cfg.expose_agent_files_data = true;

    MediatedPythonRunner runner(std::move(cfg));
    auto init = runner.initialize();
    check(init.has_value(), "setup: MediatedPythonRunner with a real materialized skill mount initializes");

    ExecState state{};
    EffectContext ctx{};
    auto granted = CapabilitySet::grant_root(
        {Capability{cap::FsRead{"real-mounted-skill", "", std::nullopt}}});
    ctx.capabilities = &granted;

    // ---- the actual proof: real Python code, real open(), real mounted skill content ---------------
    {
        ExecRequest req{"python",
                         "content = open('/real-mounted-skill/SKILL.md').read()\n"
                         "print('SKILL_CONTENT:', content)"};
        auto out = runner.run(req, state, ctx);
        check(out.has_value(), "R1: execute_code-shaped run() succeeds reading a mounted skill via open()");
        if (out) {
            check(out->stdout_text.find("real-mounted-skill") != std::string::npos &&
                      out->stdout_text.find("REAL SKILL BODY") != std::string::npos,
                  "R1: the REAL SKILL.md content (frontmatter + body) comes back through a real "
                  "mediated open() call from inside real Python code -- not the advertisement summary, "
                  "not a stub");
        }
    }

    // ---- a second, independent surface (agent.files.list) also reaches the same mount --------------
    {
        ExecRequest req{"python",
                         "from agent import files\n"
                         "entries = files.list('/real-mounted-skill')\n"
                         "print('LISTING:', entries)"};
        auto out = runner.run(req, state, ctx);
        check(out.has_value() && out->stdout_text.find("SKILL.md") != std::string::npos,
              "R2: agent.files.list also sees the real materialized skill directory -- the "
              "currently-production-disabled agent.files/agent.data surface, now enabled and reachable");
    }

    // ---- negative control: a mount the caller never granted a capability for is still denied -------
    {
        ExecRequest req{"python", "open('/real-mounted-skill/../work/nope.txt', 'r')"};
        auto out = runner.run(req, state, ctx);
        // This still targets the granted mount id (path traversal is rejected independently by
        // split_mount_path); the real cross-mount denial is proven by test_mediated_python_runner_*'s
        // own existing hostile-corpus/capability suites -- this file's own scope is proving the
        // POSITIVE path for a materialized skill mount, not re-deriving that denial coverage.
        //
        // `ExecOutcome::klass` reflects whether the RUNNER completed a well-formed run, not whether
        // guest code raised -- an uncaught Python exception (exactly like a real REPL cell) is still a
        // normal, klass::ok outcome, with the traceback delivered via stderr_text. The real signal for
        // "the guest code's own attempt was rejected" is therefore stderr_text, confirmed by manually
        // inspecting this exact call's output while writing this test: a real `OSError: '.'/'..' are
        // not meaningful in a content-addressed tree path`, raised by the worktree path parser itself,
        // before any capability check even runs.
        check(out.has_value() && (out->stderr_text.find("OSError") != std::string::npos ||
                                   out->stderr_text.find("PermissionError") != std::string::npos),
              "R3: a path-traversal attempt against the mounted skill raises a real denial (captured "
              "in stderr_text, not silently served as if it had succeeded)");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_mediated_python_runner_skill_mounts: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_mediated_python_runner_skill_mounts: %d FAILURE(S)\n", g_failures);
    return 1;
}
