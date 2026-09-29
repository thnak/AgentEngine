// Proves EffectContext::sandbox_fs (effect_context.hpp) -- the seam letting a native Tool<> reach
// a session's sandbox mount -- via the worked example tools/read_sandbox_file.hpp. First-party-
// tools follow-up ("every session gets a real sandbox; tools reach it too"), Phase 0 of
// docs' own build order for that work: mirrors how tools/read_content.hpp proved
// blob_sink/tool_result_byte_threshold end to end.
//
// Platform-portable as of 2026-08-28 (ADR-103, the Linux-parity pass): the seam itself
// (EffectContext::sandbox_fs, an abstract FileSystemAdapter*) was always platform-agnostic; the one
// real conformer this test constructs against an actual mount (MediatedFileSystemAdapter,
// src/backends/native_jail/) now has a real Linux implementation alongside the Windows one.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "agentengine/tools/read_sandbox_file.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using agentengine::tools::ReadSandboxFile;

agentengine::EffectContext make_ctx(agentengine::CapabilitySet const& held) {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);
    return ctx;
}

// A fake FileSystemAdapter (mirrors tests/tools/test_read_content.cpp's own FakeEgressBackend pattern)
// -- used ONLY to prove the negative-control case (a denied capability check never reaches the
// adapter at all), so that case doesn't depend on real filesystem I/O to observe.
class CountingFileSystemAdapter final : public agentengine::FileSystemAdapter {
public:
    mutable int read_file_calls = 0;

    agentengine::result<std::vector<std::byte>> read_file(std::string_view) override {
        ++read_file_calls;
        return std::vector<std::byte>{};
    }
    agentengine::result<void> write_file(std::string_view, std::span<std::byte const>, bool) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<void> remove(std::string_view, bool) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<void> rename(std::string_view, std::string_view) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<void> copy_file(std::string_view, std::string_view) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<void> make_directory(std::string_view, bool) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<std::vector<agentengine::DirEntry>> list_directory(std::string_view) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<bool> exists(std::string_view) override { return true; }
    agentengine::result<std::string> canonicalize(std::string_view path) override {
        return std::string(path);
    }
};

void test_default_is_nullptr_no_sandbox() {
    agentengine::CapabilitySet const held;
    auto ctx = make_ctx(held);
    check(ctx.sandbox_fs == nullptr, "EffectContext::sandbox_fs defaults to nullptr");

    auto reply = ReadSandboxFile::invoke(ReadSandboxFile::Args{"anything.txt"}, ctx);
    check(!reply.has_value(), "no sandbox_fs wired -> the tool fails, not a crash or silent no-op");
    if (!reply) {
        check(reply.error().code == "read_sandbox_file.no_sandbox",
              "the failure names exactly why: no sandbox for this session yet");
    }
}

void test_capability_denied_never_touches_adapter() {
    agentengine::CapabilitySet const held;  // no FsRead grant at all
    auto ctx = make_ctx(held);
    CountingFileSystemAdapter adapter;
    ctx.sandbox_fs = &adapter;

    auto reply = ReadSandboxFile::invoke(ReadSandboxFile::Args{"secret.txt"}, ctx);
    check(!reply.has_value(), "no FsRead grant -> denied");
    if (!reply) check(reply.error().code == "tool.capability_not_held", "denial uses the standard capability error code");
    check(adapter.read_file_calls == 0, "the adapter is never touched when the capability check fails (I2)");
}

void test_real_adapter_reads_a_real_file() {
    // std::filesystem::temp_directory_path() (portable) rather than a hand-read TEMP env var with a
    // Windows-only fallback path (2026-08-28, ADR-103, the Linux-parity pass).
    std::string const scratch =
        (std::filesystem::temp_directory_path() / "ae_sandbox_fs_seam_test").string();
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    {
        std::ofstream f(scratch + "/hello.txt", std::ios::binary);
        f << "hello from the sandbox mount";
    }

    auto adapter = agentengine::native_jail::mediated_shell::MediatedFileSystemAdapter::create(scratch);
    check(adapter.has_value(), "setup: MediatedFileSystemAdapter::create succeeds");
    if (!adapter) return;

    agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
        {agentengine::cap::FsRead{std::string(agentengine::tools::kSandboxWorkMount), "", std::nullopt}});
    auto ctx = make_ctx(held);
    ctx.sandbox_fs = &*adapter;

    auto reply = ReadSandboxFile::invoke(ReadSandboxFile::Args{"hello.txt"}, ctx);
    check(reply.has_value(), "reading a real file through the seam succeeds");
    if (reply) check(reply->content == "hello from the sandbox mount", "content matches what's really on disk");

    std::filesystem::remove_all(scratch);
}

// GitHub issue #149: `read_sandbox_file` looked the grant up but never used its `size_cap_bytes`, so a
// file past the cap was read and returned whole while the shell's `cat` refused the same file
// (`shell.cat_exceeds_size_cap`). The cap is what keeps an oversized file out of a tool result and so
// out of the model's context.
//
//   A -- a file LARGER than the granted cap is refused, with the policy code, and no content is returned.
//   B -- a file of EXACTLY the cap is returned (the bound is `>`, not `>=`; no off-by-one over-refusal).
//   C -- the SAME oversized file is returned under an uncapped grant: the cap alone decides A.
//   D -- a cap on a narrower path prefix does not apply to a file outside it (the grant found is the one
//        that covers THIS path, not the first FsRead in the set).
void test_size_cap_is_enforced() {
    std::string const scratch = (std::filesystem::temp_directory_path() / "ae_sandbox_fs_cap_test").string();
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch + "/small");
    auto const write = [&](std::string const& rel, std::size_t n) {
        std::ofstream f(scratch + "/" + rel, std::ios::binary);
        f << std::string(n, 'x');
    };
    write("big.txt", 2000);
    write("exact.txt", 1000);
    write("small/under.txt", 2000);  // in a directory whose grant has NO cap

    auto adapter = agentengine::native_jail::mediated_shell::MediatedFileSystemAdapter::create(scratch);
    check(adapter.has_value(), "setup: MediatedFileSystemAdapter::create succeeds (cap test)");
    if (!adapter) return;

    auto const mount = std::string(agentengine::tools::kSandboxWorkMount);
    {
        agentengine::CapabilitySet const capped =
            agentengine::CapabilitySet::grant_root({agentengine::cap::FsRead{mount, "", std::uint64_t{1000}}});
        auto ctx = make_ctx(capped);
        ctx.sandbox_fs = &*adapter;

        auto over = ReadSandboxFile::invoke(ReadSandboxFile::Args{"big.txt"}, ctx);
        check(!over.has_value(), "A: a 2000-byte file under a 1000-byte FsRead cap is refused");
        if (!over) {
            check(over.error().code == "read_sandbox_file.exceeds_size_cap",
                  "A: ... with read_sandbox_file.exceeds_size_cap");
            check(over.error().klass == agentengine::failure_class::policy, "A: ... as a policy failure");
        }

        auto exact = ReadSandboxFile::invoke(ReadSandboxFile::Args{"exact.txt"}, ctx);
        check(exact.has_value() && exact->content.size() == 1000,
              "B: a file of exactly the cap (1000) is returned in full");
    }
    {
        agentengine::CapabilitySet const uncapped =
            agentengine::CapabilitySet::grant_root({agentengine::cap::FsRead{mount, "", std::nullopt}});
        auto ctx = make_ctx(uncapped);
        ctx.sandbox_fs = &*adapter;
        auto big = ReadSandboxFile::invoke(ReadSandboxFile::Args{"big.txt"}, ctx);
        check(big.has_value() && big->content.size() == 2000,
              "C (control): the same 2000-byte file under an UNCAPPED grant is returned -- the cap decides A");
    }
    {
        // Two grants: a capped one on "" would also cover small/under.txt, so put the uncapped grant on
        // "small" FIRST. find_fs_read returns the first grant that covers the path; the point is that a
        // cap on a grant that does not cover the path is not applied to it.
        agentengine::CapabilitySet const two = agentengine::CapabilitySet::grant_root(
            {agentengine::cap::FsRead{mount, "small", std::nullopt},
             agentengine::cap::FsRead{mount, "elsewhere", std::uint64_t{10}}});
        auto ctx = make_ctx(two);
        ctx.sandbox_fs = &*adapter;
        auto under = ReadSandboxFile::invoke(ReadSandboxFile::Args{"small/under.txt"}, ctx);
        check(under.has_value() && under->content.size() == 2000,
              "D: a cap on a grant whose prefix does not cover the path is not applied to it");
    }

    std::filesystem::remove_all(scratch);
}

}  // namespace

int main() {
    test_default_is_nullptr_no_sandbox();
    test_capability_denied_never_touches_adapter();
    test_real_adapter_reads_a_real_file();
    test_size_cap_is_enforced();

    if (g_failures == 0) {
        std::printf("test_effect_context_sandbox_fs: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_effect_context_sandbox_fs: %d check(s) failed\n", g_failures);
    return 1;
}
