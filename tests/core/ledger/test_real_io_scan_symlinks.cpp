// GitHub issue #142: `RealIoFileSystem::scan_and_drain_into_tree()` must not follow a symbolic link the
// sandboxed command left in its working directory.
//
// Before the fix the scan classified entries with a link-following status query, so a link to an existing
// file outside the root was visited as that file, and `read_real_file()`'s containment check then failed
// the WHOLE scan. `python -m venv .venv` plants exactly that (`.venv/bin/python -> /usr/bin/python3` on
// Linux), so every commit after it failed -- and whether it failed told the container whether a host path
// existed.
//
//   L1 (positive control, the old failure) -- reading the planted link through `read_real_file()`, which
//      is what the pre-fix scan did for it, is refused. Without this, L2 passing could mean the link was
//      never a problem on this platform.
//   L2 -- the scan of the same directory SUCCEEDS, its tree holds only the real files, and the link is
//      reported root-relative in `skipped_symlinks`.
//   L3 -- a dangling link and a link to a directory are skipped and reported too, not errors.
//   L4 -- a caller that passes no list (every pre-#142 caller) still gets the successful scan.
//
// Needs permission to create symbolic links. Where that is missing (Windows without Developer Mode) it
// prints SKIP and proves nothing; the WSL/Linux build is where it runs for real. No daemon, no network.

#include "agentengine/core/ledger.hpp"
#include "agentengine/sandbox/real_io_filesystem.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace agentengine;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

[[nodiscard]] fs::path fresh_dir(std::string const& name) {
    fs::path const p = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

[[nodiscard]] std::vector<std::string> names_of(Tree const& t) {
    std::vector<std::string> out;
    for (auto const& e : t.entries) out.push_back(e.name);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle author = authority.mint_root("real-io-scan-symlinks");

    fs::path const root = fresh_dir("ae_scan_symlinks_root");
    fs::path const outside = fresh_dir("ae_scan_symlinks_outside");
    std::error_code ec;
    fs::create_directories(outside / "dir", ec);
    { std::ofstream(outside / "python3", std::ios::binary) << "an interpreter outside the sandbox root"; }
    { std::ofstream(outside / "dir" / "f.txt", std::ios::binary) << "d"; }

    RealIoFileSystem io(root);
    (void)io.write("main.py", to_bytes("print('hi')\n"));
    (void)io.write(".venv/pyvenv.cfg", to_bytes("home = /usr/bin\n"));

    // The venv shape: an ABSOLUTE link to a file that EXISTS, outside the root.
    std::error_code l1, l2, l3;
    fs::create_directories(root / ".venv" / "bin", ec);
    fs::create_symlink(outside / "python3", root / ".venv" / "bin" / "python", l1);
    fs::create_directory_symlink(outside / "dir", root / "dirlink", l2);
    fs::create_symlink(root / "nowhere", root / "dangling", l3);

    if (l1 || l2 || l3) {
        std::printf("[SKIP] cannot create symbolic links here (%s) -- #142 NOT PROVEN on this run\n",
                    (l1 ? l1 : l2 ? l2 : l3).message().c_str());
        fs::remove_all(root, ec);
        fs::remove_all(outside, ec);
        return 0;
    }

    // ---- L1: the positive control -- what the pre-fix scan did with the link.
    {
        auto read = io.read_real_file(".venv/bin/python");
        check(!read.has_value(),
              "L1 (positive control): reading the venv link through read_real_file() is refused -- the pre-#142 "
              "scan did exactly this for it, and failed the whole scan");
    }

    // ---- L2/L3: the scan itself.
    {
        Ledger<> ledger;
        std::vector<std::string> skipped;
        auto tree = drive(io.scan_and_drain_into_tree(ledger, author, &skipped));
        check(tree.has_value(), "L2: the scan of a directory holding a venv link SUCCEEDS" +
                                    (tree.has_value() ? std::string{} : " (got: " + tree.error().message + ")"));
        if (tree.has_value()) {
            check(names_of(*tree) == std::vector<std::string>{".venv/pyvenv.cfg", "main.py"},
                  "L2: ... its tree holds only the real files -- nothing read through a link");
        }
        std::sort(skipped.begin(), skipped.end());
        check(skipped == std::vector<std::string>{".venv/bin/python", "dangling", "dirlink"},
              "L2/L3: ... and the venv link, the dangling link and the directory link are each reported, "
              "root-relative");
    }

    // ---- L4: the pre-#142 call shape.
    {
        Ledger<> ledger;
        auto tree = drive(io.scan_and_drain_into_tree(ledger, author));
        check(tree.has_value(), "L4: a caller passing no list still gets a successful scan");
    }

    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
