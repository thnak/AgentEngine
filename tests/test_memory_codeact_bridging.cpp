// Proof for decisions/ADR-153-agent-memory-codeact-bridging.md -- the two pieces GitHub issue #31's
// own comment (and 026-Agent-Facing-Runtime-Surface.md §5's `agent.memory` row) named as unreachable
// from inside a CodeAct `execute_code` script even though the underlying capability is real:
//
//   1. `MemoryProvider::make_recall_tool_descriptor()` (now public) round-trips through the SAME
//      `bridge_tool_call()` mechanism `test_tool_bridge.cpp` already proves for an ordinary tool --
//      capability-gated exactly like every other bridged tool, denied without `FsRead`, allowed with
//      it, never a second, weaker check.
//   2. `native_jail::materialize_memory_mount()` puts a principal's real memory content onto a real
//      host directory, gated on a host-supplied `cap::FsRead`, with the identical reserved-mount-id
//      fail-closed guard `test_skill_mount_materializer.cpp` already proves for skills.
//
// Neither piece is a new capability or a new ambient-authority path (I2): both require a host to
// explicitly assemble a `ToolBridgeConfig`/`mount_roots` entry, exactly as `tool_bridge.hpp`'s own
// file-top comment already mandates for every OTHER bridged tool.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "agentengine/core/memory_provider.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "backends/native_jail/memory_mount_materializer.hpp"
#include "backends/native_jail/tool_bridge.hpp"

using namespace agentengine;
using namespace agentengine::native_jail;

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

class NullSummarizer {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        co_return std::unexpected(error{failure_class::contract, "unused in this test", "test.unused"});
    }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 1;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        pair.producer.close();
        return std::move(pair.consumer);
    }
};

[[nodiscard]] std::string read_file(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    InMemoryWorktreeObjectStore object_store;
    rt::InMemoryAppendLogStore ref_store;
    Principal const principal{"p-recall-test", "tenant-a"};

    auto bootstrapped = ensure_memory_worktree(object_store, ref_store, principal);
    check(bootstrapped.has_value(), "setup: the memory worktree bootstraps");

    Mount const mount = memory_mount(principal);
    cap::FsRead const read_cap{memory_mount_id(principal), "", std::nullopt};
    cap::FsWrite const write_cap{memory_mount_id(principal), "", std::nullopt, std::nullopt};

    MemoryItem note{};
    note.kind = memory_kind::semantic;
    note.content = "the user's timezone is UTC+7";
    note.tags = {"preference"};
    note.salience = 0.8f;
    note.origin = MemoryOrigin{memory_source::user_stated, "run-0", "turn-0", principal};
    check(write_memory_item(object_store, ref_store, mount, write_cap, note).has_value(),
          "setup: writing the memory item to recall/materialize succeeds");

    // ---- Piece 1: agent.tools.recall reachable through the SAME bridge as any other tool ----------
    {
        MemoryProvider<NullSummarizer, InMemoryWorktreeObjectStore, rt::InMemoryAppendLogStore> provider(
            object_store, ref_store, mount, read_cap, write_cap, NullSummarizer{});
        ToolDescriptor recall_descriptor = provider.make_recall_tool_descriptor();
        check(recall_descriptor.name == "recall", "R1: the descriptor is real and named 'recall'");

        ToolCallRequest req;
        req.tool_name  = "recall";
        req.call_id    = "c1";
        req.arguments  = json::Value::make_object({{"query", json::Value::make_string("timezone")}});
        EffectContext ctx{};

        // Negative control: no FsRead grant in the bridge config -> denied.
        {
            ToolBridgeConfig config;
            config.bridged_tools = ToolTable::from_descriptors({recall_descriptor});
            config.approved      = true;
            auto result = bridge_tool_call(config, req, ctx);
            check(result.is_error, "R1: recall is DENIED through a bridge config with no FsRead grant");
        }

        // Positive control: the SAME descriptor, now WITH the matching FsRead grant -> succeeds.
        {
            ToolBridgeConfig config;
            config.bridged_tools = ToolTable::from_descriptors({recall_descriptor});
            config.capabilities  = {Capability{read_cap}};
            config.approved      = true;
            auto result = bridge_tool_call(config, req, ctx);
            check(!result.is_error, "R1: recall SUCCEEDS through a bridge config granting the matching FsRead");
            if (!result.is_error) {
                bool found_content = false;
                for (auto const& item : result.content) {
                    if (auto const* data = std::get_if<Data>(&item.value)) {
                        if (data->json.find("UTC+7") != std::string::npos) found_content = true;
                        check(item.tainted, "R1: the recalled content re-enters tainted (003 §2)");
                    }
                }
                check(found_content, "R1: the reply actually carries the real memory item's content, "
                                       "not a stub -- a genuine round trip through the bridge");
            }
        }
    }

    // ---- Piece 2: /memory materializes to a real host directory, gated and fail-closed -----------
    {
        std::filesystem::path const scratch =
            std::filesystem::temp_directory_path() / "ae_memory_mount_materializer_test";
        std::filesystem::remove_all(scratch);
        std::filesystem::create_directories(scratch);

        // R2: a normal materialization produces the real memory content on disk.
        {
            std::filesystem::path const host_root = scratch / "r2";
            auto materialized = materialize_memory_mount(object_store, ref_store, principal, read_cap,
                                                            "memory", host_root, {"work", "input", "out"});
            check(materialized.has_value(), "R2: materialize_memory_mount succeeds for a bootstrapped "
                                              "principal with a real FsRead grant");
            if (!materialized) {
                std::fprintf(stderr, "  (R2 error: %s / %s)\n", materialized.error().code.c_str(),
                              materialized.error().message.c_str());
            }
            if (materialized) {
                check(materialized->first == "memory",
                      "R2: the returned mount name is the host-chosen presentation token, not the "
                      "colon-bearing internal mount_id");
                std::filesystem::path const item_dir(materialized->second);
                bool found = false;
                std::error_code ec;
                for (auto const& entry : std::filesystem::recursive_directory_iterator(item_dir, ec)) {
                    if (!entry.is_regular_file()) continue;
                    if (read_file(entry.path()).find("UTC+7") != std::string::npos) found = true;
                }
                check(found, "R2: the real memory item's content is a real file under the materialized "
                              "host directory -- a genuine worktree-to-host round trip");
            }
        }

        // R3: a reserved-mount-id collision fails closed with nothing written.
        {
            std::filesystem::path const host_root = scratch / "r3";
            auto materialized = materialize_memory_mount(object_store, ref_store, principal, read_cap,
                                                            "memory", host_root, {"memory"});
            check(!materialized.has_value(),
                  "R3: a reserved mount name colliding with the chosen 'memory' token is refused");
            if (!materialized) {
                check(materialized.error().code == "memory.mount_id_reserved",
                      "R3: the failure carries the specific, named reserved-collision error code");
            }
            check(!std::filesystem::exists(host_root),
                  "R3: NOTHING was written to disk for this call -- fails closed before any directory "
                  "creation, not partway through");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_memory_codeact_bridging: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_memory_codeact_bridging: %d FAILURE(S)\n", g_failures);
    return 1;
}
