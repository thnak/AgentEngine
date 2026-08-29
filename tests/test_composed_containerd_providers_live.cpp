// ADR-106 (decisions/ADR-106-containerd-execution-surface-promotion.md) §7 residual, closed here: the
// ContainerdExecutionSurface counterpart to test_composed_sandbox_providers_live.cpp (that file's own
// original ADR-102 Phase 5 slice). Proves the SAME claim that file proves for Docker, but for
// containerd: ComposedContextProvider<SandboxToolProvider, MandatorySandboxProvider<
// ContainerdExecutionSurface>>, composed as a REAL, production agentengine::rt::AgentSession<...>'s
// actual HistoryProviderT, driven through the real, unmodified session.start_run() -> invoke_tool()
// 10-step pipeline -- the first time MandatorySandboxProvider<ContainerdExecutionSurface> specifically
// (as opposed to <DockerExecutionSurface>) is proven to work through that real pipeline, not just
// standalone against ContainerdExecutionSurface directly the way
// tests/test_containerd_execution_surface.cpp already does.
//
// Near-verbatim port of test_composed_sandbox_providers_live.cpp -- see that file's own top comment
// for the full reasoning behind every structural decision reused here unchanged (the bind_sandbox()-
// before-engage() ordering requirement, the ScriptedChatClient fixture shape, the independent
// ledger/filesystem read-back-after-the-fact verification style). This file's only real differences:
// the ExecutionSurface template argument, and requiring `ctr`/containerd instead of `docker`.
//
// REQUIRES: Linux, a running containerd daemon reachable via the `ctr` CLI on PATH (root or an
// unprivileged containerd-socket ACL -- matches tests/test_containerd_execution_surface.cpp's own
// disclosed precondition).

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/sandbox/containerd_execution_surface.hpp"
#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"
#include "backends/native_jail/sandbox_tool_provider.hpp"

#include "agentengine/rt/agent_session.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace agentengine;

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

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message text_message(std::string text) {
    Message m;
    m.role = role::assistant;
    m.message_id = "m-text";
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message tool_call_message(std::string call_id, std::string tool_name, std::string args_json) {
    Message m;
    m.role = role::assistant;
    m.message_id = "m-" + call_id;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = ToolCall{std::move(call_id), std::move(tool_name), std::move(args_json),
                            content_origin::assistant, call_provenance::vendor_structured};
    m.content.push_back(item);
    return m;
}

// Same shape as test_composed_sandbox_providers_live.cpp's own tool_reply_json_of().
std::optional<std::string> tool_reply_json_of(Message const& m) {
    for (ContentItem const& item : m.content) {
        auto const* tr = std::get_if<ToolResult>(&item.value);
        if (!tr || tr->is_error || tr->content.empty()) continue;
        auto const* d = std::get_if<Data>(&tr->content[0].value);
        if (!d) continue;
        return d->json;
    }
    return std::nullopt;
}

// Same shape as test_composed_sandbox_providers_live.cpp's own ScriptedChatClient.
class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<Message> script;
        std::size_t call_count = 0;
    };
    void set_script(std::vector<Message> script) { state_->script = std::move(script); }
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    agentengine::task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        Message reply = (state_->call_count < state_->script.size()) ? state_->script[state_->call_count]
                                                                        : text_message("done");
        ++state_->call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    std::shared_ptr<State> state_;
};
static_assert(agentengine::ChatClient<ScriptedChatClient>);

using ComposedProvider =
    ComposedContextProvider<SandboxToolProvider, MandatorySandboxProvider<ContainerdExecutionSurface>>;
static_assert(agentengine::ContextProvider<ComposedProvider>,
              "ComposedContextProvider<SandboxToolProvider, MandatorySandboxProvider<Surface>> must "
              "itself satisfy ContextProvider -- the same composition rule any Ms... pack follows, "
              "regardless of which ExecutionSurface Surface actually is");
using Session = agentengine::rt::AgentSession<ScriptedChatClient, agentengine::rt::NoSessionState, ComposedProvider>;

// Same shape as test_composed_sandbox_providers_live.cpp's own session_digest_of().
[[nodiscard]] std::optional<std::string> session_digest_of(std::string_view session_id) {
    std::vector<std::byte> bytes(session_id.size());
    for (std::size_t i = 0; i < session_id.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(session_id[i]));
    }
    auto digest = compute_digest(bytes);
    if (!digest.has_value()) return std::nullopt;
    return *digest;
}

// Same shape as test_composed_sandbox_providers_live.cpp's own read_ledger_entry().
std::optional<std::string> read_ledger_entry(Ledger<>& ledger, std::string const& branch_name,
                                                IdentityHandle owner, std::string const& entry_name) {
    auto head = ledger.head_tree_digest(branch_name, owner);
    if (!head.has_value()) return std::nullopt;
    auto tree = ledger.get_tree_safe(*head, owner);
    if (!tree.has_value()) return std::nullopt;
    for (auto const& e : tree->entries) {
        if (e.name != entry_name) continue;
        auto bytes = ledger.get_blob_safe(e.digest, owner);
        if (!bytes.has_value()) return std::nullopt;
        return std::string(reinterpret_cast<char const*>(bytes->data()), bytes->size());
    }
    return std::nullopt;
}

}  // namespace

int main() {
    namespace json = agentengine::json;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal real_owner_principal = make_embedded_principal("composed-containerd-providers-test-owner");
    IdentityHandle owner = authority.adopt(real_owner_principal);
    Ledger<> ledger;
    auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    check(storage_quota_r.has_value(), "setup: AsyncQuota<StorageBytes>::mint_root(owner) succeeds");
    if (!storage_quota_r.has_value()) return EXIT_FAILURE;
    auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    check(run_quota_r.has_value(), "setup: AsyncQuota<RunCost>::mint_root(owner) succeeds");
    if (!run_quota_r.has_value()) return EXIT_FAILURE;
    auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    check(branch_quota_r.has_value(), "setup: AsyncQuota<BranchCost>::mint_root(owner) succeeds");
    if (!branch_quota_r.has_value()) return EXIT_FAILURE;
    auto& storage_quota = *storage_quota_r;
    auto& run_quota = *run_quota_r;
    auto& branch_quota = *branch_quota_r;

    std::filesystem::path const scratch_root =
        std::filesystem::temp_directory_path() / "ae_test_composed_containerd_providers";
    std::error_code ec;
    std::filesystem::remove_all(scratch_root, ec);
    std::filesystem::path const shell_scratch_root = scratch_root / "shell";
    std::filesystem::path const ledger_staging_root = scratch_root / "ledger";

    auto root_r = drive(ledger.create_root_branch(owner, "composed-containerd-live"));
    check(root_r.has_value(), "setup: create_root_branch(owner, \"composed-containerd-live\") succeeds");
    if (!root_r.has_value()) return EXIT_FAILURE;
    std::string const branch_name = root_r->name();

    MandatorySandboxProvider<ContainerdExecutionSurface> sandbox_provider;
    sandbox_provider.bind_sandbox(ledger, std::move(*root_r), owner, ledger_staging_root, branch_quota,
                                    run_quota, storage_quota);
    SandboxToolProvider shell_provider(shell_scratch_root.string());

    Session live;
    live.initialize("composed-containerd-live-session", real_owner_principal);
    auto engaged = live.history_provider().engage(
        std::make_tuple(std::move(shell_provider), std::move(sandbox_provider)));
    check(engaged.has_value(), "setup: engage() succeeds with both real, already-bound providers");
    if (!engaged.has_value()) return EXIT_FAILURE;

    CapabilitySet const held = CapabilitySet::grant_root({
        Capability{cap::FsRead{"work", "", std::nullopt}},
        Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
    });
    live.set_capabilities(&held);

    // [1] one on_context() call from the composed provider contributes BOTH tools.
    {
        std::vector<Message> empty_history;
        SessionContext session_ctx{"composed-containerd-live-session", real_owner_principal, empty_history};
        EffectContext direct_ctx;
        direct_ctx.principal = real_owner_principal;
        direct_ctx.capabilities = borrow_capabilities(held);

        auto contribution = drive(live.history_provider().on_context(session_ctx, direct_ctx));
        check(contribution.has_value(), "[1] on_context() on the composed provider succeeds");
        if (contribution.has_value()) {
            check(contribution->tools.size() == 2,
                  "[1] the composed provider contributes exactly two tools");
            bool saw_shell = false, saw_command = false;
            for (auto const& t : contribution->tools) {
                if (t.name == "run_shell") saw_shell = true;
                if (t.name == "run_command") saw_command = true;
            }
            check(saw_shell, "[1] run_shell (SandboxToolProvider) is one of the two contributed tools");
            check(saw_command,
                  "[1] run_command (MandatorySandboxProvider) is the other contributed tool");
        }
    }

    // [2]/[3]: run_command, then run_shell, each driven through the REAL invoke_tool() pipeline via
    // ONE start_run() round, in the SAME composed session.
    {
        ScriptedChatClient& client = live.emplace_chat_client();
        std::string const command_args = json::dump(json::Value::make_object(
            {{"command", json::Value::make_string(
                             "echo -n 'from composed run_command' > composed_command.txt && "
                             "cat composed_command.txt")}}));
        std::string const shell_args = json::dump(json::Value::make_object(
            {{"source",
              json::Value::make_string("echo from-composed-run-shell > composed_shell.txt")}}));
        client.set_script({tool_call_message("call-1", "run_command", command_args),
                             tool_call_message("call-2", "run_shell", shell_args)});

        auto response = drive(live.start_run(agentengine::rt::StartRun{user_message("go")}));
        check(response.has_value(),
              "[2]/[3] start_run() driving both a run_command and a run_shell tool call through "
              "invoke_tool(), in the same composed session, succeeds");

        bool found_command_result = false;
        for (Message const& m : live.history()) {
            if (m.role != role::tool) continue;
            auto reply_json = tool_reply_json_of(m);
            if (!reply_json.has_value()) continue;
            if (reply_json->find("\"ok\":true") != std::string::npos &&
                reply_json->find("from composed run_command") != std::string::npos) {
                found_command_result = true;
            }
        }
        check(found_command_result,
              "[2] run_command genuinely executed in a real containerd container through the composed "
              "session's real invoke_tool() pipeline, with SandboxToolProvider also composed in");

        auto ledger_entry = read_ledger_entry(ledger, branch_name, owner, "composed_command.txt");
        check(ledger_entry.has_value() && *ledger_entry == "from composed run_command",
              "[2] the real Ledger checkpoint invoke_tool() committed contains the real file, read "
              "back independently of run_command's own reported reply");

        auto shell_session_digest = session_digest_of("composed-containerd-live-session");
        check(shell_session_digest.has_value(), "[3] setup: session_digest_of() succeeds");
        std::error_code shell_out_ec;
        std::filesystem::path const shell_out =
            shell_scratch_root / shell_session_digest.value_or("") / "composed_shell.txt";
        check(shell_session_digest.has_value() && std::filesystem::exists(shell_out, shell_out_ec),
              "[3] run_shell genuinely executed against the real host filesystem in the same composed "
              "session -- the file SandboxToolProvider's own jail wrote is on disk, independently of "
              "run_shell's own reported reply, and independently of MandatorySandboxProvider's "
              "completely separate Ledger-backed storage checked just above");
    }

    std::filesystem::remove_all(scratch_root, ec);

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- ComposedContextProvider<SandboxToolProvider, "
                     "MandatorySandboxProvider<ContainerdExecutionSurface>> composes as the REAL, "
                     "production AgentSession's actual HistoryProviderT, driven end to end through "
                     "the real, unmodified session.start_run() -> invoke_tool() 10-step pipeline, for "
                     "BOTH a native-jail run_shell call and a containerd-backed run_command call in one "
                     "session -- the first real production use of "
                     "MandatorySandboxProvider<ContainerdExecutionSurface> through that pipeline "
                     "anywhere in this codebase.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
