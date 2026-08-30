// Proves the A10 promotion (decisions/ADR-114-task-branch-tools-promotion.md) --
// MandatorySandboxProvider<Surface>'s four new task-branch verbs (start_task_branch/
// run_in_task_branch/commit_task_branch/discard_task_branch, sandbox/mandatory_sandbox_provider.hpp),
// ported from docs/planning/proofs/task_branch_tool/task_branch_sandbox.hpp -- the FIRST real
// production caller of SandboxRuntime::merge_into() anywhere in this codebase.
//
// REQUIRES a running Docker daemon reachable via the `docker` CLI on PATH -- every check below shells
// out to a REAL container. Follows tests/test_mandatory_sandbox_provider.cpp's own established rigor
// bar: direct-accessor proofs for the state-machine/error-path claims, PLUS at least one proof driven
// through the REAL, unmodified session.start_run() -> invoke_tool() pipeline (this design's own prior
// red-team history shows direct-accessor-only proofs get flagged as insufficient).
//
//   [1] the opt-in gate: bind_sandbox() alone contributes ONLY run_command; bind_task_branch_tools()
//       is a real, separate, required second step before the four task-branch tools appear at all.
//   [2] start -> run -> commit: a real child branch is forked, a real command runs on it, and
//       committing genuinely merges the result into the PARENT's own branch -- the parent can read a
//       file it never wrote itself, proving "the orchestrator resumes" for real, not just structurally.
//       Also proves the MergeCost quota is consumed by exactly 1 unit on a successful commit.
//   [3] best-of-N: two children forked from the SAME unmoved parent head; committing the first is a
//       clean fast-forward, and the second (still based on the original head) is then a genuine
//       CONFLICT once it also tries to commit -- rejected, but the SAME handle_id stays usable
//       afterward (re-surfaced via the real Ledger orphan-reclaim path), not "unknown handle."
//   [4] discard refunds the BranchCost unit start_task_branch spent, and the handle becomes genuinely
//       unusable afterward.
//   [5] every verb fails closed with task_branch.unknown_handle for a handle_id that was never issued
//       (or was already consumed by a prior commit/discard).
//   [6] driven through the REAL invoke_tool() pipeline: a ScriptedChatClient issues start/run/commit
//       as three real tool calls; session.history() shows all three real role::tool results, and the
//       committed file is independently readable on the parent's branch afterward.

#include "agentengine/sandbox/docker_execution_surface.hpp"
#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"

#include "agentengine/rt/agent_session.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
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

class ScriptedChatClient {
public:
    ScriptedChatClient() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<Message> script;
        std::size_t call_count = 0;
    };
    // Unlike test_mandatory_sandbox_provider.cpp's own copy of this fixture (which only ever drives
    // ONE start_run() per session), this test drives THREE successive script phases on the SAME
    // session -- set_script() alone would leave call_count from the PRIOR phase's own round-trips
    // (a tool call plus the "done" text fallback both increment it) pointing past this phase's own
    // script entirely. Reset explicitly at each new phase boundary.
    void set_script(std::vector<Message> script) {
        state_->script = std::move(script);
        state_->call_count = 0;
    }
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

using Provider = MandatorySandboxProvider<DockerExecutionSurface>;
using Session = agentengine::rt::AgentSession<ScriptedChatClient, agentengine::rt::NoSessionState, Provider>;

struct RealCallContext {
    Principal real_principal;
    CapabilitySet real_caps;
    EffectContext ctx;
    std::vector<Message> empty_history;
    SessionContext session_ctx;

    RealCallContext(std::string label, Principal principal)
        : real_principal(std::move(principal)), real_caps(CapabilitySet::grant_root({})),
          session_ctx(std::move(label), real_principal, empty_history) {
        ctx.principal = real_principal;
        ctx.capabilities = borrow_capabilities(real_caps);
    }
};

std::optional<std::string> read_entry(Ledger<>& ledger, std::string const& branch_name,
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
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal real_owner_principal = make_embedded_principal("task-branch-tools-test-owner");
    IdentityHandle owner = authority.adopt(real_owner_principal);
    Ledger<> ledger;
    auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    auto merge_quota_r = agentengine::rt::AsyncQuota<MergeCost>::mint_root(authority, owner, 100);
    check(storage_quota_r.has_value() && run_quota_r.has_value() && branch_quota_r.has_value() &&
              merge_quota_r.has_value(),
          "all four quota mint_root() calls succeed");
    if (!storage_quota_r.has_value() || !run_quota_r.has_value() || !branch_quota_r.has_value() ||
        !merge_quota_r.has_value()) {
        return EXIT_FAILURE;
    }
    auto& storage_quota = *storage_quota_r;
    auto& run_quota = *run_quota_r;
    auto& branch_quota = *branch_quota_r;
    auto& merge_quota = *merge_quota_r;

    std::filesystem::path const scratch_root =
        std::filesystem::temp_directory_path() / "ae_test_task_branch_tools";
    std::error_code ec;
    std::filesystem::remove_all(scratch_root, ec);

    RealCallContext call("task-branch-tools-test", real_owner_principal);

    // [1] the opt-in gate.
    Session parent;
    parent.initialize("parent-session", real_owner_principal);
    {
        auto root_r = drive(ledger.create_root_branch(owner, "tb-parent"));
        check(root_r.has_value(), "create_root_branch(owner, \"tb-parent\") succeeds");
        if (!root_r.has_value()) return EXIT_FAILURE;
        parent.history_provider().bind_sandbox(ledger, std::move(*root_r), owner, scratch_root / "parent",
                                                  branch_quota, run_quota, storage_quota);

        auto before = drive(parent.history_provider().on_context(call.session_ctx, call.ctx));
        check(before.has_value() && before->tools.size() == 1 && before->tools[0].name == "run_command",
              "bind_sandbox() alone contributes ONLY run_command -- task-branch tools stay opted out");

        parent.history_provider().bind_task_branch_tools(merge_quota);
        auto after = drive(parent.history_provider().on_context(call.session_ctx, call.ctx));
        check(after.has_value() && after->tools.size() == 5,
              "bind_task_branch_tools() makes on_context() contribute all 5 tools");
        if (after.has_value() && after->tools.size() == 5) {
            bool has_start = false, has_run = false, has_commit = false, has_discard = false;
            for (auto const& t : after->tools) {
                has_start |= (t.name == "start_task_branch");
                has_run |= (t.name == "run_in_task_branch");
                has_commit |= (t.name == "commit_task_branch");
                has_discard |= (t.name == "discard_task_branch");
            }
            check(has_start && has_run && has_commit && has_discard,
                  "all four task-branch tool names are present");
        }
    }

    // [2] start -> run -> commit: real merge into the parent's own branch, MergeCost consumed by 1.
    {
        std::uint64_t const merge_before = merge_quota.remaining();
        std::uint64_t const branch_before = branch_quota.remaining();

        auto started = drive(parent.history_provider().start_task_branch(owner));
        check(started.has_value(), "start_task_branch() succeeds");
        if (!started.has_value()) return EXIT_FAILURE;
        std::string const handle = started->handle_id;
        check(branch_quota.remaining() == branch_before - 1,
              "start_task_branch() consumes exactly 1 BranchCost unit");

        auto ran = drive(parent.history_provider().run_in_task_branch(
            handle, "echo -n 'from a task branch' > from_child.txt", owner));
        check(ran.has_value(), "run_in_task_branch() succeeds");

        auto committed = drive(parent.history_provider().commit_task_branch(handle, owner));
        check(committed.has_value(), "commit_task_branch() succeeds (no conflict, clean fast-forward)");
        check(merge_quota.remaining() == merge_before - 1,
              "a successful commit_task_branch() consumes exactly 1 MergeCost unit");

        // THE central claim: the file the CHILD wrote is now readable on the PARENT's own branch --
        // "the orchestrator resumes" for real, driven through the tool surface, not just Ledger::merge()
        // in isolation (ADR-112 already proved that half; this proves a real caller reaches it).
        std::string const parent_branch = parent.history_provider().runtime()->branch_name();
        auto entry = read_entry(ledger, parent_branch, owner, "from_child.txt");
        check(entry.has_value() && *entry == "from a task branch",
              "the parent's own branch now contains the child's committed file");

        // The handle is one-shot: a second commit on the SAME handle must fail unknown_handle.
        auto second_commit = drive(parent.history_provider().commit_task_branch(handle, owner));
        check(!second_commit.has_value() &&
                  second_commit.error().code == "mandatory_sandbox_provider.task_branch_unknown_handle",
              "committing an already-committed handle fails task_branch_unknown_handle");
    }

    // [3] best-of-N: two children from the SAME unmoved head; first commits clean, second conflicts
    //     and is reclaimed under the SAME handle rather than lost.
    {
        auto child_a = drive(parent.history_provider().start_task_branch(owner));
        auto child_b = drive(parent.history_provider().start_task_branch(owner));
        check(child_a.has_value() && child_b.has_value(), "both best-of-N children start successfully");
        if (!child_a.has_value() || !child_b.has_value()) return EXIT_FAILURE;

        auto ran_a = drive(parent.history_provider().run_in_task_branch(
            child_a->handle_id, "echo -n 'A wins' > contested.txt", owner));
        auto ran_b = drive(parent.history_provider().run_in_task_branch(
            child_b->handle_id, "echo -n 'B loses' > contested.txt", owner));
        check(ran_a.has_value() && ran_b.has_value(), "both children can run independently (isolated)");

        auto commit_a = drive(parent.history_provider().commit_task_branch(child_a->handle_id, owner));
        check(commit_a.has_value(), "the FIRST commit (still based on the current head) is a clean merge");

        auto commit_b = drive(parent.history_provider().commit_task_branch(child_b->handle_id, owner));
        check(!commit_b.has_value(),
              "the SECOND commit (now stale -- parent moved under it) is a real, rejected conflict");

        // The SAME handle_id must still work -- reclaimed, not stranded (task_branch_sandbox.hpp's own
        // A10 finding 3, now proven through this real tool surface for the first time).
        auto retry_run = drive(parent.history_provider().run_in_task_branch(
            child_b->handle_id, "echo -n 'still alive' > still_alive.txt", owner));
        check(retry_run.has_value(),
              "the rejected handle_id is reclaimed and stays usable -- NOT unknown_handle");

        auto discard_b = drive(parent.history_provider().discard_task_branch(child_b->handle_id));
        check(discard_b.has_value(), "the reclaimed handle can be cleanly discarded afterward");

        std::string const parent_branch = parent.history_provider().runtime()->branch_name();
        auto contested = read_entry(ledger, parent_branch, owner, "contested.txt");
        check(contested.has_value() && *contested == "A wins",
              "the parent's branch reflects A's committed content, not B's rejected one");
    }

    // [4] discard refunds BranchCost and genuinely invalidates the handle.
    {
        std::uint64_t const branch_before = branch_quota.remaining();
        auto started = drive(parent.history_provider().start_task_branch(owner));
        check(started.has_value(), "start_task_branch() for the discard check succeeds");
        if (!started.has_value()) return EXIT_FAILURE;
        check(branch_quota.remaining() == branch_before - 1, "starting spends 1 BranchCost unit");

        auto discarded = drive(parent.history_provider().discard_task_branch(started->handle_id));
        check(discarded.has_value(), "discard_task_branch() succeeds");
        check(branch_quota.remaining() == branch_before,
              "a successful discard refunds the BranchCost unit back to the pre-start value");

        auto after_discard = drive(
            parent.history_provider().run_in_task_branch(started->handle_id, "echo hi", owner));
        check(!after_discard.has_value() &&
                  after_discard.error().code == "mandatory_sandbox_provider.task_branch_unknown_handle",
              "a discarded handle is genuinely gone -- run_in_task_branch on it fails unknown_handle");
    }

    // [5] every verb fails closed on a handle_id that was never issued.
    {
        auto bogus_run =
            drive(parent.history_provider().run_in_task_branch("no-such-handle", "echo hi", owner));
        check(!bogus_run.has_value() &&
                  bogus_run.error().code == "mandatory_sandbox_provider.task_branch_unknown_handle",
              "run_in_task_branch() on a never-issued handle fails unknown_handle");
        auto bogus_commit = drive(parent.history_provider().commit_task_branch("no-such-handle", owner));
        check(!bogus_commit.has_value() &&
                  bogus_commit.error().code == "mandatory_sandbox_provider.task_branch_unknown_handle",
              "commit_task_branch() on a never-issued handle fails unknown_handle");
        auto bogus_discard = drive(parent.history_provider().discard_task_branch("no-such-handle"));
        check(!bogus_discard.has_value() &&
                  bogus_discard.error().code == "mandatory_sandbox_provider.task_branch_unknown_handle",
              "discard_task_branch() on a never-issued handle fails unknown_handle");
    }

    // [5b] every verb fails closed if bind_task_branch_tools() was never called, even when bound.
    {
        Session unenabled;
        unenabled.initialize("unenabled-session", Principal{"unenabled-owner", ""});
        auto root_r = drive(ledger.create_root_branch(owner, "tb-unenabled"));
        check(root_r.has_value(), "create_root_branch(owner, \"tb-unenabled\") succeeds");
        if (root_r.has_value()) {
            unenabled.history_provider().bind_sandbox(ledger, std::move(*root_r), owner,
                                                          scratch_root / "unenabled", branch_quota,
                                                          run_quota, storage_quota);
            auto attempt = drive(unenabled.history_provider().start_task_branch(owner));
            check(!attempt.has_value() &&
                      attempt.error().code == "mandatory_sandbox_provider.task_branch_not_enabled",
                  "start_task_branch() on a bound-but-not-enabled provider fails task_branch_not_enabled");
        }
    }

    // [6] driven through the REAL invoke_tool() pipeline: start/run/commit as three real tool calls.
    {
        Session live;
        live.initialize("live-session", real_owner_principal);
        auto root_r = drive(ledger.create_root_branch(owner, "tb-live"));
        check(root_r.has_value(), "create_root_branch(owner, \"tb-live\") succeeds");
        if (root_r.has_value()) {
            live.history_provider().bind_sandbox(ledger, std::move(*root_r), owner, scratch_root / "live",
                                                     branch_quota, run_quota, storage_quota);
            live.history_provider().bind_task_branch_tools(merge_quota);

            ScriptedChatClient& client = live.emplace_chat_client();
            std::string const start_args = json::dump(json::Value::make_object(
                {{"label", json::Value::make_string("pipeline test")}}));
            client.set_script({tool_call_message("call-start", "start_task_branch", start_args)});

            CapabilitySet const held = CapabilitySet::grant_root({});
            live.set_capabilities(&held);

            auto start_response = drive(live.start_run(agentengine::rt::StartRun{user_message("go")}));
            check(start_response.has_value(),
                  "start_run() driving a real start_task_branch tool call through invoke_tool() succeeds");

            std::string handle_id;
            for (Message const& m : live.history()) {
                if (m.role != role::tool) continue;
                auto reply_json = tool_reply_json_of(m);
                if (!reply_json.has_value()) continue;
                auto parsed = json::parse(*reply_json);
                if (parsed.has_value() && parsed->is_object()) {
                    auto const* h = parsed->find("handle_id");
                    if (h && h->is_string()) handle_id = h->as_string();
                }
            }
            check(!handle_id.empty(),
                  "session.history() contains a real start_task_branch reply carrying a handle_id");

            if (!handle_id.empty()) {
                std::string const run_args = json::dump(json::Value::make_object(
                    {{"handle_id", json::Value::make_string(handle_id)},
                     {"command", json::Value::make_string(
                                     "echo -n 'from pipeline task branch' > pipeline_child.txt")}}));
                std::string const commit_args = json::dump(
                    json::Value::make_object({{"handle_id", json::Value::make_string(handle_id)}}));
                client.set_script({tool_call_message("call-run", "run_in_task_branch", run_args),
                                     tool_call_message("call-commit", "commit_task_branch", commit_args)});

                auto run_response = drive(live.start_run(agentengine::rt::StartRun{user_message("go")}));
                check(run_response.has_value(), "the pipeline-driven run_in_task_branch call succeeds");
                auto commit_response = drive(live.start_run(agentengine::rt::StartRun{user_message("go")}));
                check(commit_response.has_value(), "the pipeline-driven commit_task_branch call succeeds");

                std::string const live_branch = live.history_provider().runtime()->branch_name();
                auto entry = read_entry(ledger, live_branch, owner, "pipeline_child.txt");
                check(entry.has_value() && *entry == "from pipeline task branch",
                      "the file committed through the REAL tool-call pipeline is really on the parent's "
                      "own branch, read back independently of any tool's own reported reply");
            }
        }
    }

    std::filesystem::remove_all(scratch_root, ec);

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- MandatorySandboxProvider's task-branch tools give "
                     "SandboxRuntime::merge_into() its first real production caller, proven through "
                     "direct calls, a best-of-N/conflict-reclaim sequence, and the real, unmodified "
                     "session.start_run() -> invoke_tool() pipeline, against a REAL Docker daemon.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
