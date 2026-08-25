#pragma once
// Track A follow-up ("every session gets a real sandbox; tools reach it too"): the worked
// example proving `EffectContext::sandbox_fs` end to end, the same role `read_content.hpp`
// played for `blob_sink`/`tool_result_byte_threshold`.
//
// A minimal native `Tool<>` (006 §2's "Native" source) reading one file from the session's own
// sandbox mount via `EffectContext::sandbox_fs` -- proof the seam works, not a general-purpose
// sandbox file tool (009 §7's own generic `read_content`-class candidate already covers the
// broader "read from any granted source" case once a future pass extends `read_content.hpp` with
// a worktree-path source, per that file's own file-top comment naming this as deferred work).
//
// Fixed to the "work" mount by convention (`tools/cli_chat.cpp`'s own `kWorkMount`, the
// established name for a session's primary sandbox mount) -- `sandbox_fs` is inherently
// one-mount-per-session in the shape this seam is first wired against (session_shell_wiring.hpp),
// so there is no second mount id a caller could even mean here yet.
//
// I2: `Capabilities<>` is deliberately empty -- `sandbox_fs` grants nothing by itself (see its own
// EffectContext comment). This tool performs its OWN dynamic capability check against
// `ctx.capabilities->find_fs_read("work", path)` before touching the adapter, the same pattern
// `tools/read_content.hpp` already establishes for a capability whose target isn't knowable at a
// tool's compile-time declaration.

#include <string>
#include <string_view>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::tools {

inline constexpr std::string_view kSandboxWorkMount = "work";

// ae-naming-lint: allow ReadSandboxFileArgs — new tool, matches every other Args type's own naming
struct ReadSandboxFileArgs {
    std::string path;  // relative to the "work" mount's own root
};
AE_JSON_SCHEMA(ReadSandboxFileArgs, path)

// ae-naming-lint: allow ReadSandboxFileReply — new tool, matches every other Reply type's own naming
struct ReadSandboxFileReply {
    std::string content;
};
AE_JSON_SCHEMA(ReadSandboxFileReply, content)

struct ReadSandboxFile : Tool<ReadSandboxFile, Capabilities<>, Approval<approval_mode::never_require>,
                               EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "read_sandbox_file";
    static constexpr std::string_view description =
        "Read one file from this session's own sandbox mount, if it has one.";

    using Args = ReadSandboxFileArgs;
    using Reply = ReadSandboxFileReply;

    [[nodiscard]] static result<Reply> invoke(Args args, EffectContext& ctx) {
        if (!ctx.sandbox_fs) {
            return std::unexpected(error{failure_class::resource,
                                          "this session has no sandbox mount yet",
                                          "read_sandbox_file.no_sandbox"});
        }
        if (!ctx.capabilities) {
            return std::unexpected(error{failure_class::policy, "no capability set bound to this call",
                                          "tool.capability_not_held"});
        }
        auto granted = ctx.capabilities->find_fs_read(std::string(kSandboxWorkMount), args.path);
        if (!granted) {
            return std::unexpected(error{failure_class::policy,
                                          "no granted FsRead capability covers '" + args.path +
                                              "' on the '" + std::string(kSandboxWorkMount) + "' mount",
                                          "tool.capability_not_held"});
        }

        auto bytes = ctx.sandbox_fs->read_file(args.path);
        if (!bytes) return std::unexpected(bytes.error());

        Reply reply;
        reply.content.assign(reinterpret_cast<char const*>(bytes->data()), bytes->size());
        return reply;
    }
};

}  // namespace agentengine::tools
