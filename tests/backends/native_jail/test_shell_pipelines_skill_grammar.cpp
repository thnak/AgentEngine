// GitHub issue #47, deliverable 3: the `shell-pipelines` built-in skill (core/builtin_skills.hpp) must
// describe the mediated shell `run_shell` ACTUALLY runs (mediated_shell_parser.cpp,
// mediated_shell_dispatch.cpp, session_shell_wiring.hpp) -- not a shell it once planned to be. Each
// G-check below is one claim the skill text makes, run through the real `run_shell` descriptor and the
// real 006 §3 pipeline (`invoke_tool`), under the grants tools/cli_chat.cpp gives its mediated tier
// (FsRead/FsWrite on "work", nothing else). G0 additionally runs every ```sh block in the skill and
// fails if any no longer runs cleanly, so an example cannot rot silently.
//
// If one of these fails after a shell change, the skill text is now wrong: fix the text (and this test)
// together, never just the test.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "agentengine/core/builtin_skills.hpp"
#include "agentengine/core/skill.hpp"
#include "backends/native_jail/session_shell_wiring.hpp"

namespace {

namespace ae = agentengine;
namespace json = agentengine::json;

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// What tools/cli_chat.cpp's mediated tier holds for "work": exactly these two, uncapped.
ae::CapabilitySet cli_chat_work_grants() {
    return ae::CapabilitySet::grant_root({
        ae::Capability{ae::cap::FsRead{"work", "", std::nullopt}},
        ae::Capability{ae::cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
    });
}

struct Run {
    bool tool_error = false;  // the call failed as a whole (parse error, refused permission)
    std::string error_code;
    bool ok = false;          // RunShellReply::ok -- the last statement's verdict
    std::string out;
    std::string err;
};

class Shell {
public:
    explicit Shell(std::filesystem::path root) : root_(std::move(root)) {
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        auto made = ae::SessionShellSandbox::create(root_);
        if (made) sandbox_ = std::move(*made);
    }
    ~Shell() {
        sandbox_.reset();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    [[nodiscard]] bool ready() const { return sandbox_ != nullptr; }
    [[nodiscard]] std::filesystem::path const& root() const { return root_; }

    Run run(std::string const& source, ae::CapabilitySet const& held) {
        auto const table = ae::ToolTable::from_descriptors({sandbox_->tool_descriptor()});
        ae::EffectContext ctx;
        ctx.principal = ae::Principal{"skill-grammar", ""};
        ctx.capabilities = ae::borrow_capabilities(held);
        ae::ToolCallRequest const req{"c" + std::to_string(++n_), "run_shell",
                                      json::Value::make_object({{"source", json::Value::make_string(source)}}),
                                      false};
        ae::ToolInvocationAudit audit;
        ae::ToolResult const r = ae::invoke_tool(table, held, req, ctx, nullptr, &audit);
        Run out;
        if (r.is_error) {
            out.tool_error = true;
            out.error_code = audit.error_code;
            return out;
        }
        for (ae::ContentItem const& item : r.content) {
            if (auto const* d = std::get_if<ae::Data>(&item.value)) {
                auto parsed = json::parse(d->json);
                if (!parsed) continue;
                if (auto const* v = parsed->find("ok")) out.ok = v->as_bool();
                if (auto const* v = parsed->find("stdout_text")) out.out = v->as_string();
                if (auto const* v = parsed->find("stderr_text")) out.err = v->as_string();
            }
        }
        return out;
    }
    Run run(std::string const& source) { return run(source, held_); }

private:
    std::filesystem::path root_;
    std::unique_ptr<ae::SessionShellSandbox> sandbox_;
    ae::CapabilitySet held_ = cli_chat_work_grants();
    int n_ = 0;
};

std::string read_file(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// Every ```sh ... ``` block in the skill text, in order.
std::vector<std::string> sh_blocks(std::string_view md) {
    std::vector<std::string> out;
    std::string_view const open = "```sh\n";
    std::size_t pos = 0;
    while ((pos = md.find(open, pos)) != std::string_view::npos) {
        std::size_t const start = pos + open.size();
        std::size_t const end = md.find("```", start);
        if (end == std::string_view::npos) break;
        out.emplace_back(md.substr(start, end - start));
        pos = end + 3;
    }
    return out;
}

}  // namespace

int main() {
    std::filesystem::path const base = std::filesystem::temp_directory_path() / "ae_shell_skill_grammar";

    // ---- The skill itself still parses, and is the v2 text this test was written against -----------
    {
        auto skill = ae::parse_skill_md(ae::kShellPipelinesSkillMd, "shell-pipelines");
        check(skill.has_value(), "S1: the shell-pipelines skill text parses as a SKILL.md");
        check(skill && skill->frontmatter.description.find("run_shell") != std::string::npos,
              "S1: its description names the run_shell tool it teaches");
    }

    // ---- G0: every ```sh example in the skill runs cleanly, in order, in one session ---------------
    {
        Shell sh(base / "g0");
        check(sh.ready(), "G0 setup: a SessionShellSandbox over a real directory");
        auto const blocks = sh_blocks(ae::kShellPipelinesSkillMd);
        check(blocks.size() >= 2, "G0: the skill carries runnable ```sh examples");
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            Run const r = sh.run(blocks[i]);
            if (r.tool_error) std::fprintf(stderr, "    block %zu failed: %s\n", i, r.error_code.c_str());
            check(!r.tool_error && r.ok, "G0: a ```sh example from the skill runs to completion, ok");
        }
        if (!blocks.empty()) {
            check(read_file(sh.root() / "notes" / "a.txt") == "first line\nsecond line\n",
                  "G0: the first example's > then >> left both lines in notes/a.txt, as the skill says");
        }
    }

    Shell sh(base / "g");
    if (!sh.ready()) {
        std::fprintf(stderr, "setup failed\n");
        return 1;
    }

    // G1: newline separates statements; the reply carries every statement's output.
    {
        Run const r = sh.run("echo one\necho two; echo three");
        check(!r.tool_error && r.ok && r.out == "one\ntwo\nthree\n",
              "G1: newline and ';' both separate statements, and all their output comes back");
        Run const c = sh.run("# a comment\necho after-comment");
        check(c.out == "after-comment\n", "G1: '#' starts a comment");
    }

    // G2: cd persists between calls; `cd /` is the work root.
    {
        (void)sh.run("mkdir g2dir");
        (void)sh.run("cd g2dir");
        check(sh.run("pwd").out == "/g2dir\n", "G2: a cd in one call is still in effect in the next");
        (void)sh.run("cd /");
        check(sh.run("pwd").out == "/\n", "G2: 'cd /' returns to the work directory's root");
    }

    // G3: NAME=value cmd sets NAME for that command line only; nothing persists without export.
    {
        check(sh.run("X=1 echo $X").out == "1\n", "G3: 'NAME=value cmd' sets NAME for that command");
        check(sh.run("echo [$X]").out == "[]\n", "G3: ...and it does not persist to the next call");
    }

    // G4: a bare NAME=value is a parse error, and a parse error runs nothing.
    {
        Run const r = sh.run("echo before > g4.txt\nX=1");
        check(r.tool_error && r.error_code == "shell.parse_error", "G4: a bare 'NAME=value' is a parse error");
        check(!std::filesystem::exists(sh.root() / "g4.txt"),
              "G4: a parse error rejects the whole call before anything runs (no file written)");
    }

    // G5: export needs an EnvWrite grant cli_chat does not mint; refused, it stops the whole call.
    {
        Run const r = sh.run("echo a > g5.txt\nexport X=1\necho b > g5b.txt");
        check(r.tool_error && r.error_code == "shell.capability_denied",
              "G5: under cli_chat's grants, export is refused and the call fails");
        check(std::filesystem::exists(sh.root() / "g5.txt") && !std::filesystem::exists(sh.root() / "g5b.txt"),
              "G5: statements before the refusal ran, nothing after it did");
        // Positive control: the grant, not the syntax, is what decides.
        ae::CapabilitySet const with_env = ae::CapabilitySet::grant_root({
            ae::Capability{ae::cap::FsRead{"work", "", std::nullopt}},
            ae::Capability{ae::cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
            ae::Capability{ae::cap::EnvWrite{"X"}},
        });
        Run const ok = sh.run("export X=1", with_env);
        check(!ok.tool_error && ok.ok, "G5 control: with EnvWrite{X} granted, export succeeds");
        check(sh.run("echo $X", with_env).out == "1\n", "G5 control: ...and an exported variable persists");
    }

    // G6: quotes are literal -- double quotes too -- and expansion outside quotes works.
    {
        Run const r = sh.run("Y=v echo \"$Y\" '$Y' $Y ${Y}");
        check(r.out == "$Y $Y v v\n", "G6: \"$Y\" and '$Y' stay literal; $Y and ${Y} expand");
    }

    // G7: an unknown command is an ordinary failure -- reported, and the script goes on.
    {
        Run const r = sh.run("grep x f\necho after");
        check(!r.tool_error && r.ok && r.out == "after\n", "G7: an unknown command does not stop the script");
        check(r.err.find("grep: command not found") != std::string::npos,
              "G7: ...and is reported as 'command not found'");
    }

    // G8: && / || branch on success, and a line may end in || and continue on the next.
    {
        check(sh.run("cat nope.txt || echo fallback").out == "fallback\n", "G8: || runs on failure");
        check(sh.run("cat nope.txt && echo never").out.empty(), "G8: && skips on failure");
        check(sh.run("cat nope.txt ||\necho continued").out == "continued\n",
              "G8: a line ending in || continues on the next");
    }

    // G9: if/else/fi and for/do/done, on one line or several; the condition is a command's success.
    {
        check(sh.run("for f in a b c; do echo item $f; done").out == "item a\nitem b\nitem c\n",
              "G9: for ... in ...; do ...; done");
        check(sh.run("if ls g2dir; then\n  echo yes\nelse\n  echo no\nfi").out == "yes\n",
              "G9: multi-line if, condition true");
        check(sh.run("if ls nodir; then echo yes; else echo no; fi").out == "no\n",
              "G9: one-line if, condition false (no test/[ builtin needed)");
        Run const t = sh.run("test -f x");
        check(!t.tool_error && !t.ok, "G9: there is no 'test' -- it is an unknown command");
    }

    // G10: pipes -- only cat reads its input.
    {
        check(sh.run("echo hi | cat").out == "hi\n", "G10: 'echo hi | cat' passes the text through");
        check(sh.run("echo hi | echo x").out == "x\n", "G10: a builtin other than cat ignores its input");
    }

    // G11: > and >>; '2>' is not a stderr redirect, the 2 is an argument.
    {
        (void)sh.run("echo a 2> g11.txt");
        check(read_file(sh.root() / "g11.txt") == "a 2\n", "G11: in 'echo a 2> f', the 2 is an argument");
    }

    // G12: no command substitution, no globbing.
    {
        check(sh.run("echo $(pwd)").out == "$(pwd)\n", "G12: $(...) is plain text");
        check(sh.run("echo `pwd`").out == "`pwd`\n", "G12: backticks are plain text");
        check(sh.run("echo *").out == "*\n", "G12: * is a literal character");
        Run const amp = sh.run("echo a &");
        check(amp.tool_error && amp.error_code == "shell.parse_error", "G12: a bare & is a parse error");
    }

    // G13: builtin flags -- echo has none, ls has none, rm takes only -r, mkdir takes -p.
    {
        check(sh.run("echo -n x").out == "-n x\n", "G13: echo prints -n");
        Run const ls = sh.run("ls -la");
        check(!ls.tool_error && !ls.ok, "G13: 'ls -la' reads -la as a path and fails");
        (void)sh.run("mkdir -p g13/a/b");
        check(std::filesystem::is_directory(sh.root() / "g13" / "a" / "b"), "G13: mkdir -p makes parents");
        (void)sh.run("rm -rf g13");
        check(std::filesystem::exists(sh.root() / "g13"), "G13: 'rm -rf' is read as a path -- nothing removed");
        (void)sh.run("rm -r g13");
        check(!std::filesystem::exists(sh.root() / "g13"), "G13: 'rm -r' removes a directory");
        check(sh.run("ls").out.find("g2dir/\n") != std::string::npos, "G13: ls marks directories with '/'");
    }

    // G14: a redirect that cannot be written stops the whole call (the refused-export case is G5); a
    // read of the same out-of-bounds kind is only an ordinary failure.
    {
        Run const w = sh.run("echo visible > g14a.txt\necho x > ../escape.txt\necho after > g14b.txt");
        check(w.tool_error, "G14: a '>' outside the work directory fails the whole call");
        check(std::filesystem::exists(sh.root() / "g14a.txt") && !std::filesystem::exists(sh.root() / "g14b.txt"),
              "G14: statements before it ran, nothing after it did");
        check(!std::filesystem::exists(sh.root().parent_path() / "escape.txt"), "G14: and nothing escaped");
        Run const r = sh.run("cat ../../x.txt\necho after");
        check(!r.tool_error && r.ok && r.out == "after\n",
              "G14 control: an out-of-bounds READ is an ordinary failure; the script continues");
    }

    if (g_failures == 0) {
        std::printf("test_shell_pipelines_skill_grammar: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_shell_pipelines_skill_grammar: %d check(s) failed\n", g_failures);
    return 1;
}
