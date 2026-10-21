// Proves decisions/ADR-244-required-choices-step-1b.md G-C5(e) by reading the source: every AppContainer grant the
// engine makes goes through `grant_spec_mounts` in src/backends/native_jail/native_jail_backend.cpp, the one place
// that runs after `authorize_spec` and the deployment-path checks. A grant call anywhere else in src/, include/ or
// tools/ is a path that reaches the file system without them.
//
//   S1 (positive control) the scan finds the grant calls inside `grant_spec_mounts`, so it reads the right file and
//      recognises a call.
//   S2 no grant call exists outside it, apart from the definitions in app_container_profile.{hpp,cpp}.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
void check(bool cond, std::string const& what) {
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "  ok" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

std::string read_file(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// Grant calls are `.grant_path(` / `grant_path(` on a profile and the free `grant_sid_path(`.
constexpr std::string_view kTokens[] = {"grant_path(", "grant_sid_path("};

std::size_t count_calls(std::string_view text) {
    std::size_t n = 0;
    for (std::string_view token : kTokens) {
        for (std::size_t at = text.find(token); at != std::string_view::npos; at = text.find(token, at + 1)) {
            // Skip mentions inside comments: the line's text before the token holds `//`.
            std::size_t const line_start = text.rfind('\n', at) == std::string_view::npos ? 0 : text.rfind('\n', at) + 1;
            if (text.substr(line_start, at - line_start).find("//") != std::string_view::npos) continue;
            ++n;
        }
    }
    return n;
}

// The body of `grant_spec_mounts`, by brace matching from its definition.
std::string_view grant_spec_mounts_body(std::string_view text) {
    std::size_t const def = text.find("result<void> grant_spec_mounts(");
    if (def == std::string_view::npos) return {};
    std::size_t const open = text.find('{', text.find(')', def));
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        if (text[i] == '}' && --depth == 0) return text.substr(open, i - open + 1);
    }
    return {};
}

}  // namespace

int main() {
    fs::path const root = AE_SOURCE_ROOT;
    std::string const backend = read_file(root / "src/backends/native_jail/native_jail_backend.cpp");
    std::string_view const body = grant_spec_mounts_body(backend);
    std::size_t const inside = count_calls(body);
    check(!body.empty() && inside >= 3, "S1 (positive control): grant calls are found inside grant_spec_mounts (" +
                                            std::to_string(inside) + ")");

    std::vector<std::string> strays;
    for (char const* dir : {"src", "include", "tools"}) {
        for (auto const& entry : fs::recursive_directory_iterator(root / dir)) {
            if (!entry.is_regular_file()) continue;
            std::string const ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc") continue;
            std::string const name = entry.path().filename().string();
            if (name == "app_container_profile.cpp" || name == "app_container_profile.hpp") continue;
            std::string const text = read_file(entry.path());
            std::size_t calls = count_calls(text);
            if (name == "native_jail_backend.cpp") calls -= inside;
            if (calls != 0) strays.push_back(fs::relative(entry.path(), root).generic_string() + " (" + std::to_string(calls) + ")");
        }
    }
    for (std::string const& s : strays) std::fprintf(stderr, "  grant call outside grant_spec_mounts: %s\n", s.c_str());
    check(strays.empty(), "S2: no AppContainer grant call outside grant_spec_mounts");

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
