#pragma once
// ADR-096 (decisions/ADR-096-session-sandbox-lifecycle-context-provider-wiring.md), Design B:
// `SandboxToolProvider` -- the `ContextProvider` conformer (`core/context_provider.hpp`) that gives
// Shell a real, reusable, per-session lifecycle. Composed into a session via the existing
// `Ms.../ComposedContextProvider<Ms...>` mechanism (`core/composed_context_provider.hpp`,
// ADR-074) -- no new `AgentSession` mutator, no new builder step (ADR-096 §3 C1).
//
// On its own first `on_context()` call this provider lazily constructs one `SessionShellSandbox`
// (`session_shell_wiring.hpp`, Tier-1 of the "every session gets a real sandbox" roadmap,
// C:\Users\thanh\.claude\plans\resilient-yawning-cook.md) rooted at a per-session subdirectory of a
// host-supplied scratch root, then on EVERY `on_context()` call (this one included) it (a)
// contributes `run_shell`'s `ToolDescriptor` into `ContextContribution.tools` and (b) assigns
// `ctx.sandbox_fs` via the mutable `EffectContext&` parameter -- the exact seam
// `EffectContext::sandbox_fs` was added for (ADR-096 §2 Design B).
//
// Non-copyable by construction (holds `std::unique_ptr<SessionShellSandbox>`, itself non-movable-
// after-construction per that class's own comment -- so this provider is move-only, never copyable,
// with no operations declared here at all; the implicitly-deleted copy is the whole point). Composing
// this into a session's `HistoryProviderT` makes `AgentSession::fork_from()` a COMPILE ERROR for that
// session type (ADR-096 C2, empirically proven against `ComposedContextProvider<Ms...>`'s own
// move-only fix, ADR-074) -- accepted, not a defect: a fork must never alias one session's live
// sandbox filesystem view with another's.
//
// Per-session subdirectory naming (ADR-096 §3 C8): `compute_digest(session_id_bytes)`
// (`core/worktree.hpp`) is ALREADY hex-encoded -- no separate `to_hex()` step -- then validated with
// an explicit `[0-9a-f]`/non-empty character-class check before being spliced into a host path,
// mirroring `agent_spawn_worktree.hpp`'s own "WT-6" precedent for an identically-shaped hazard (a
// provably-hex-only value still gets defense-in-depth validation, not just trusted by construction).
//
// Directory creation (ADR-096 §8 residual, closed here): `MediatedFileSystemAdapter::create()`
// requires its root to already exist (`SessionShellSandbox::create()`'s own comment: "the caller owns
// creating the directory on disk") -- this provider creates it idempotently on first use, mirroring
// `tools/cli_chat.cpp`'s own `std::filesystem::create_directories(scratch, ec)` idiom
// (`tools/cli_chat.cpp:239`). A leftover directory from an abandoned prior session sharing the same
// digest (impossible unless `session_id` itself repeats -- the digest is deterministic) is reused
// as-is, the same posture `create_directories` on an already-existing path already has (a no-op, not
// an error).
//
// Windows-only for this pass, matching `SessionShellSandbox`/`MediatedFileSystemAdapter`'s own
// current platform scope (021 §2's Windows-now/Linux-next ordering).

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/worktree.hpp"
#include "backends/native_jail/session_shell_wiring.hpp"

namespace agentengine {

namespace sandbox_tool_provider_detail {

// ADR-096 §3 C8 / "WT-6" precedent (`agent_spawn_worktree.hpp::check_child_id`) -- a local,
// independent copy rather than a cross-include of that file's own `agent_spawn_worktree_detail`
// namespace: same defense-in-depth shape, applied here to an unrelated feature's own digest, not a
// shared utility the two were ever meant to depend on each other for. Stricter than that precedent
// in one respect (a fixed-length check, per ADR-096 C8's own text): `compute_digest` always emits a
// 64-char lowercase-hex SHA-256 digest, so this is additional defense-in-depth, never a real
// rejection path through the front door.
inline constexpr std::size_t kSessionDigestHexLength = 64;

[[nodiscard]] inline result<void> check_session_digest(std::string_view digest) {
    if (digest.size() != kSessionDigestHexLength) {
        return std::unexpected(error{failure_class::contract,
                                      "session sandbox digest must be exactly 64 lowercase hex chars",
                                      "sandbox_tool_provider.digest_invalid"});
    }
    for (char c : digest) {
        bool const is_lower_hex_digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!is_lower_hex_digit) {
            return std::unexpected(error{
                failure_class::contract,
                "session sandbox digest must be lowercase hex -- refuses to splice an unexpected "
                "character into a host scratch directory path",
                "sandbox_tool_provider.digest_invalid"});
        }
    }
    return {};
}

}  // namespace sandbox_tool_provider_detail

// ae-naming-lint: allow SandboxToolProvider — ADR-096's own vocabulary, the design's central type
class SandboxToolProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3.
    static constexpr std::string_view name = "sandbox";  // ae-naming-lint: allow name — ADR-033's HasMiddlewareName precedent, reused verbatim per ADR-066 §3

    // `host_scratch_root`: a real host directory (need not already exist -- created idempotently,
    // see file-top comment) under which this provider creates exactly one per-session subdirectory.
    // Host-only, configuration-time value (I2/I3: never derived from model output).
    explicit SandboxToolProvider(std::filesystem::path host_scratch_root)
        : host_scratch_root_(std::move(host_scratch_root)) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx,
                                                                  EffectContext& ctx) {
        if (!sandbox_) {
            if (auto ensured = ensure_sandbox(session_ctx.session_id); !ensured) {
                co_return std::unexpected(ensured.error());
            }
        }
        ctx.sandbox_fs = sandbox_->filesystem_adapter();
        ContextContribution contribution;
        contribution.tools.push_back(sandbox_->tool_descriptor());
        co_return contribution;
    }

    // No per-turn state to react to (`SessionShellSandbox`'s own `ExecState` persists inside itself,
    // read fresh on the next `on_context()` call via the same live `sandbox_` -- nothing here needs
    // to inspect the turn that just ended).
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    [[nodiscard]] result<void> ensure_sandbox(std::string_view session_id) {
        std::vector<std::byte> bytes(session_id.size());
        for (std::size_t i = 0; i < session_id.size(); ++i) {
            bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(session_id[i]));
        }
        auto digest = compute_digest(bytes);
        if (!digest) return std::unexpected(digest.error());
        if (auto valid = sandbox_tool_provider_detail::check_session_digest(*digest); !valid) {
            return std::unexpected(valid.error());
        }

        std::filesystem::path const session_dir = host_scratch_root_ / *digest;
        std::error_code ec;
        std::filesystem::create_directories(session_dir, ec);
        if (ec) {
            return std::unexpected(error{failure_class::fatal,
                                          "failed to create session sandbox directory: " + ec.message(),
                                          "sandbox_tool_provider.mkdir_failed"});
        }

        auto created = SessionShellSandbox::create(session_dir.wstring());
        if (!created) return std::unexpected(created.error());
        sandbox_ = std::move(*created);
        return {};
    }

    std::filesystem::path host_scratch_root_;
    std::unique_ptr<SessionShellSandbox> sandbox_;
};

}  // namespace agentengine
