// Proves EffectContext::sandbox_fs (effect_context.hpp) -- the seam letting a native Tool<> reach
// a session's sandbox mount -- via the worked example tools/read_sandbox_file.hpp. First-party-
// tools follow-up ("every session gets a real sandbox; tools reach it too"), Phase 0 of
// docs' own build order for that work: mirrors how tools/read_content.hpp proved
// blob_sink/tool_result_byte_threshold end to end.
//
// Windows-only this pass: the one real FileSystemAdapter conformer available to construct against
// an actual mount (MediatedFileSystemAdapter, src/backends/native_jail/) is Windows-only today
// (matches every other native_jail-dependent test's own scope) -- the seam itself (EffectContext::
// sandbox_fs, an abstract FileSystemAdapter*) is platform-agnostic.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "agentengine/pal/env.hpp"
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

// A fake FileSystemAdapter (mirrors tests/test_read_content.cpp's own FakeEgressBackend pattern)
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
    std::string const scratch =
        agentengine::pal::env_var("TEMP").value_or("C:/Windows/Temp") + "/ae_sandbox_fs_seam_test";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    {
        std::ofstream f(scratch + "/hello.txt", std::ios::binary);
        f << "hello from the sandbox mount";
    }
    std::wstring const scratch_w(scratch.begin(), scratch.end());

    auto adapter = agentengine::native_jail::mediated_shell::MediatedFileSystemAdapter::create(scratch_w);
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

}  // namespace

int main() {
    test_default_is_nullptr_no_sandbox();
    test_capability_denied_never_touches_adapter();
    test_real_adapter_reads_a_real_file();

    if (g_failures == 0) {
        std::printf("test_effect_context_sandbox_fs: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_effect_context_sandbox_fs: %d check(s) failed\n", g_failures);
    return 1;
}
