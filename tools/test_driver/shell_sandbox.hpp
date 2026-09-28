#pragma once
// Implements ADR-208 (decisions/ADR-208-test-driver-real-tools-and-tool-doubles.md) §2.3, G6: the one place a
// driver session's real sandbox is built. `agentengine_test_driver` and the driver's tests wire
// `make_shell_sandbox` as `DriverConfig::sandbox_factory`; the scenario runner does not include this header
// and does not link the mediated shell, so a replay cannot build a sandbox (it serves recorded doubles).
//
// The sandbox is the mediated shell (ADR-096, ADR-103): a host-side interpreter with a fixed command
// registry over `MediatedFileSystemAdapter`, confined to the session's own scratch directory, starting no
// OS process. Each run_shell call gets kRealToolWallClock (ADR-208 G1/G2).

#include <filesystem>
#include <memory>
#include <utility>

#include "backends/native_jail/session_shell_wiring.hpp"
#include "test_driver/test_driver.hpp"

namespace agentengine::test_driver {

class ShellSandbox final : public RealToolSandbox {
public:
    explicit ShellSandbox(std::unique_ptr<SessionShellSandbox> sandbox) : sandbox_(std::move(sandbox)) {}
    [[nodiscard]] ToolDescriptor run_shell() override { return sandbox_->tool_descriptor(); }
    [[nodiscard]] FileSystemAdapter* filesystem() override { return sandbox_->filesystem_adapter(); }

private:
    std::unique_ptr<SessionShellSandbox> sandbox_;
};

[[nodiscard]] inline result<std::shared_ptr<RealToolSandbox>> make_shell_sandbox(std::filesystem::path const& dir) {
    auto made = SessionShellSandbox::create(dir, kRealToolWallClock);
    if (!made) return std::unexpected(made.error());
    return std::shared_ptr<RealToolSandbox>(std::make_shared<ShellSandbox>(std::move(*made)));
}

}  // namespace agentengine::test_driver
