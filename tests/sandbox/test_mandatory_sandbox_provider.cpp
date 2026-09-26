// Proves ADR-102 Phase 4 (decisions/ADR-102-identity-native-sandbox-implementation-phase-1.md's own
// roadmap) -- MandatorySandboxProvider<Surface>/RunCommandTool (sandbox/mandatory_sandbox_provider.hpp)
// composed as a REAL, production agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>'s
// actual HistoryProviderT, ported from docs/planning/proofs/mandatory_sandbox/
// probe_mandatory_sandbox_real_agent_session.cpp (ADR-099's own standalone, red-teamed,
// live-Docker-tested prove-phase original).
//
// REQUIRES a running Docker daemon reachable via the `docker` CLI on PATH -- every check below shells
// out to a REAL container.
//
//   [1] a freshly-default-constructed REAL AgentSession has NO execution capability -- on_context()
//       (driven through the real history_provider() accessor) contributes zero tools.
//   [2] bind_sandbox() on the real session's history_provider() establishes a real sandbox; a DIRECT
//       on_context()+invoke() call (the established prove-phase proof shape) genuinely executes a real
//       command in a real container and commits a real Ledger checkpoint.
//   [3] THE NEW CLAIM this phase exists to prove, not previously exercised anywhere in this design's
//       history (prove-phase included -- every prior proof only drove MandatorySandboxProvider through
//       direct history_provider() accessor calls): a run_command tool call driven through the REAL,
//       UNMODIFIED session.start_run() -> invoke_tool() 10-step pipeline (core/tool_pipeline.hpp),
//       triggered by a ScriptedChatClient returning a real ToolCall message, actually executes inside a
//       real container and commits a real checkpoint -- confirmed by reading the reply back out of
//       session.history()'s own role::tool message, not by calling anything on the provider directly.
//   [4] two sequential children forked from the SAME bound parent via the REAL AgentSession::
//       fork_from() are mutually isolated (each session's own file is invisible on the other's branch).
//   [5] the REAL clear_in_process_state() relinquishes the branch and leaves the session re-bindable.
//   [6] would_fork_succeed() reflects real, live BranchCost quota exhaustion once a REAL fork_from()
//       call actually spends it.
//   [7] ADR-119's new static Capabilities<cap::decl::RunCommand> ceiling fails closed through the real
//       pipeline: bind_sandbox() alone (the identity/quota gate) is not enough -- a session with no
//       cap::RunCommand grant gets run_command rejected as a role::tool error, spends no RunCost, and
//       never touches the real filesystem.

#include "agentengine/sandbox/docker_execution_surface.hpp"
#include "agentengine/sandbox/mandatory_sandbox_provider.hpp"

#include "agentengine/rt/agent_session.hpp"

#include "../support/image_provenance_shape.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace agentengine;

namespace {

int g_failures = 0;
// Takes a `std::string` so a check can name WHICH thing failed rather than only that something
// did -- ADR-176 §14's gate reports the offending tool in its message, and a `char const*` would
// have thrown that away at the call site.
void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
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

// Extracts run_command's own RunCommandReply JSON fields back out of a real role::tool history
// message -- the black-box way to observe a real invoke_tool()-driven call's outcome after the fact,
// matching tests/rt/agent_session/test_rt_agent_session_tooling_and_delegation.cpp's own counter_total_of() shape.
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

// A queue of pre-built responses, consumed in order -- same shape as tests/test_rt_agent_session_
// tooling_and_delegation.cpp's own ScriptedChatClient fixture (no shared header exports this; every
// test file that needs one defines its own local copy).
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

using Provider = MandatorySandboxProvider<DockerExecutionSurface>;
static_assert(agentengine::ContextProvider<Provider>);
using Session = agentengine::rt::AgentSession<ScriptedChatClient, agentengine::rt::NoSessionState, Provider>;

// A real, minimal SessionContext/EffectContext pair for the direct-accessor checks ([1]/[2]), matching
// the prove-phase original's own RealCallContext shape.
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
    Principal real_owner_principal = make_embedded_principal("mandatory-sandbox-provider-test-owner");
    IdentityHandle owner = authority.adopt(real_owner_principal);
    Ledger<> ledger;
    auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    check(storage_quota_r.has_value(), "AsyncQuota<StorageBytes>::mint_root(owner) succeeds");
    if (!storage_quota_r.has_value()) return EXIT_FAILURE;
    auto run_quota_r = agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    check(run_quota_r.has_value(), "AsyncQuota<RunCost>::mint_root(owner) succeeds");
    if (!run_quota_r.has_value()) return EXIT_FAILURE;
    auto branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    check(branch_quota_r.has_value(), "AsyncQuota<BranchCost>::mint_root(owner) succeeds");
    if (!branch_quota_r.has_value()) return EXIT_FAILURE;
    auto& storage_quota = *storage_quota_r;
    auto& run_quota = *run_quota_r;
    auto& branch_quota = *branch_quota_r;

    std::filesystem::path const scratch_root =
        std::filesystem::temp_directory_path() / "ae_test_mandatory_sandbox_provider";
    std::error_code ec;
    std::filesystem::remove_all(scratch_root, ec);

    RealCallContext call("mandatory-sandbox-provider-test", real_owner_principal);

    // [1] a freshly-default-constructed REAL AgentSession has NO execution capability.
    {
        Session bare;
        bare.initialize("bare-session", Principal{"bare-owner", ""});
        auto contribution = drive(bare.history_provider().on_context(call.session_ctx, call.ctx));
        check(!bare.history_provider().is_bound(), "a bare session's provider reports unbound");
        check(contribution.has_value(), "on_context() on an unbound provider still succeeds");
        if (contribution.has_value()) {
            check(contribution->tools.empty(), "an unbound provider contributes zero tools");
        }
    }

    // [2] bind_sandbox() establishes a real sandbox; a direct on_context()+invoke() genuinely executes.
    Session parent;
    parent.initialize("parent-session", Principal{"parent-owner", ""});
    {
        auto root_r = drive(ledger.create_root_branch(owner, "parent"));
        check(root_r.has_value(), "create_root_branch(owner, \"parent\") succeeds");
        if (!root_r.has_value()) return EXIT_FAILURE;
        parent.history_provider().bind_sandbox(ledger, std::move(*root_r), owner, scratch_root / "parent",
                                                  branch_quota, run_quota, storage_quota);
        check(parent.history_provider().is_bound(), "bind_sandbox() leaves the provider bound");

        auto contribution = drive(parent.history_provider().on_context(call.session_ctx, call.ctx));
        check(contribution.has_value(), "on_context() on a bound provider succeeds");
        if (contribution.has_value() && !contribution->tools.empty()) {
            check(contribution->tools.size() == 1, "a bound provider contributes exactly one tool");
            check(contribution->tools[0].name == "run_command", "the contributed tool is run_command");
            // ADR-176 §14/§15. The same gate test_task_branch_tools runs over all five tools, run here
            // over the one a provider contributes without the task-branch opt-in -- the surface most
            // hosts actually get. NO exemptions: the only tool here runs a command in a container, so
            // there is nothing to excuse. Its controls live in `test_image_provenance_shape`.
            auto const shape = agentengine::test_support::image_provenance_shape(contribution->tools);
            check(shape.ok,
                  "ADR-176 §14: run_command's reply carries `image`, `image_digest` and "
                  "`image_digest_kind` as required strings -- " +
                      (shape.ok ? std::string("it conforms") : shape.detail));

            auto args = json::Value::make_object(
                {{"command", json::Value::make_string(
                                 "echo -n 'from parent, direct call' > parent.txt && cat parent.txt")}});
            auto reply = contribution->tools[0].invoke(args, call.ctx);
            check(reply.has_value(), "a direct run_command invoke() succeeds");
            if (reply.has_value()) {
                std::string const reply_json = json::dump(*reply);
                check(reply_json.find("\"ok\":true") != std::string::npos,
                      "the direct reply reports ok:true");
                check(reply_json.find("from parent, direct call") != std::string::npos,
                      "the direct reply's stdout contains the real command's real output");
                // Issue #80: the reply names the environment that produced those bytes, not only the
                // bytes. `image` is the surface's configured reference; `image_digest` is what it
                // resolved to, cross-checked against the SAME surface's own accessor rather than a
                // literal -- a hardcoded expectation here would still pass if both were fabricated.
                auto const img = parent.history_provider().bound_image();
                check(img.reference == "alpine:latest",
                      "issue #80: the bound provider reports the surface's configured image reference");
                check(img.digest.size() == 7 + 64 && img.digest.rfind("sha256:", 0) == 0,
                      "issue #80: the bound provider reports a well-formed resolved image digest");
                check(reply_json.find("\"image\":\"alpine:latest\"") != std::string::npos,
                      "issue #80: the direct reply's JSON carries the image reference");
                check(reply_json.find("\"image_digest\":\"" + img.digest + "\"") != std::string::npos,
                      "issue #80: the direct reply's JSON carries the SAME resolved digest the surface "
                      "reports");
                // ADR-176 §14 changed what the two checks above PROVE, without changing a character of
                // them. `run_command`'s body no longer fills these three fields at all -- it returns a
                // reply with them default-empty, and `with_image_provenance()` stamps them after the
                // body returns. So a broken stamp now shows up right here as an empty `image_digest`
                // against a well-formed `img.digest` -- a positive control the mechanism did not have
                // while the body stamped itself.
                check(!img.digest.empty(),
                      "ADR-176 §14 CONTROL: the digest the stamp had to carry is non-empty on this "
                      "daemon, so the check above can actually tell a stamped reply from an unstamped "
                      "one");
                // ADR-176 §9: the digest's KIND rides with it into the reply. Without this a consumer
                // has to infer comparability from the surface type, which is exactly how two digests of
                // different kinds get compared and report "different image" for one image.
                // Not a hardcoded kind: which kind is reachable depends on whether this daemon exposes a
                // descriptor media type (CI's Linux Docker does not), and pinning the VALUE here would
                // duplicate test_execution_surface_image_identity's N11 badly. What this test owns is the
                // PLUMBING -- that whatever the surface reports arrives in the reply unchanged, and that
                // the provider never invents a kind of its own.
                check(img.digest_kind.empty() || img.digest_kind == "index" ||
                          img.digest_kind == "manifest" || img.digest_kind == "config",
                      "ADR-176 §9: the bound provider reports a kind from the closed set, or empty for "
                      "not-known -- never a fabricated spelling");
                check(reply_json.find("\"image_digest_kind\":\"" + img.digest_kind + "\"") !=
                          std::string::npos,
                      "ADR-176 §9: the direct reply's JSON carries the SAME kind the surface reports, "
                      "next to the digest it describes");
            }
        }
    }

    // [3] THE NEW CLAIM: drive run_command through the REAL invoke_tool() pipeline via start_run().
    {
        Session live;
        // Deliberately `real_owner_principal`, NOT a fresh, unrelated Principal: the REAL invoke_tool()
        // pipeline derives `ctx.principal` from THIS session's own `effect_context_.principal` (set
        // here), unlike checks [1]/[2]/[4]-[6] above, which call the provider directly with an
        // externally-built EffectContext and so never actually exercise this. `IdentityAuthority::
        // adopt()` must resolve to the SAME `owner` identity the branch/quotas were minted for, or
        // AsyncQuota::try_consume() correctly refuses an unrelated spender (async_quota.
        // unauthorized_spender) -- confirmed the hard way: an earlier version of this test initialized
        // `live` with an unrelated Principal{"live-owner", ""} and this exact check failed for that
        // real reason, not a defect in MandatorySandboxProvider itself.
        live.initialize("live-session", real_owner_principal);
        auto root_r = drive(ledger.create_root_branch(owner, "live"));
        check(root_r.has_value(), "create_root_branch(owner, \"live\") succeeds");
        if (root_r.has_value()) {
            live.history_provider().bind_sandbox(ledger, std::move(*root_r), owner, scratch_root / "live",
                                                     branch_quota, run_quota, storage_quota);

            ScriptedChatClient& client = live.emplace_chat_client();
            std::string const args_json = json::dump(json::Value::make_object(
                {{"command", json::Value::make_string(
                                 "echo -n 'from real invoke_tool pipeline' > pipeline.txt && "
                                 "cat pipeline.txt")}}));
            client.set_script({tool_call_message("call-1", "run_command", args_json)});

            // ADR-119: run_command now carries a real Capabilities<cap::decl::RunCommand> ceiling.
            CapabilitySet const held = CapabilitySet::grant_root({Capability{cap::RunCommand{}}});
            live.set_capabilities(&held);

            auto response = drive(live.start_run(agentengine::rt::StartRun{user_message("go")}));
            check(response.has_value(),
                  "start_run() driving a real run_command tool call through invoke_tool() succeeds");

            bool found_real_pipeline_result = false;
            for (Message const& m : live.history()) {
                if (m.role != role::tool) continue;
                auto reply_json = tool_reply_json_of(m);
                if (!reply_json.has_value()) continue;
                if (reply_json->find("\"ok\":true") != std::string::npos &&
                    reply_json->find("from real invoke_tool pipeline") != std::string::npos) {
                    found_real_pipeline_result = true;
                }
            }
            check(found_real_pipeline_result,
                  "session.history() contains a real role::tool message proving the command genuinely "
                  "executed in a real container, driven end to end through session.start_run() -> the "
                  "real, unmodified invoke_tool() 10-step pipeline -- never a direct accessor call");

            auto pipeline_entry = read_entry(ledger, live.history_provider().runtime()->branch_name(),
                                                owner, "pipeline.txt");
            check(pipeline_entry.has_value() && *pipeline_entry == "from real invoke_tool pipeline",
                  "the real Ledger checkpoint invoke_tool() committed contains the real file, read back "
                  "independently of the tool's own reported reply");
        }
    }

    // [4] two sequential children forked from the SAME bound parent are mutually isolated.
    {
        Session sibling_a;
        sibling_a.initialize("sibling-a", Principal{"sibling-a-owner", ""});
        sibling_a.fork_from(parent, "sibling-a-session");
        check(sibling_a.history_provider().is_bound(), "sibling_a's fork_from() leaves it bound");

        Session sibling_b;
        sibling_b.initialize("sibling-b", Principal{"sibling-b-owner", ""});
        sibling_b.fork_from(parent, "sibling-b-session");
        check(sibling_b.history_provider().is_bound(), "sibling_b's fork_from() leaves it bound");

        if (sibling_a.history_provider().is_bound() && sibling_b.history_provider().is_bound()) {
            std::string const branch_a = sibling_a.history_provider().runtime()->branch_name();
            std::string const branch_b = sibling_b.history_provider().runtime()->branch_name();
            check(branch_a != branch_b, "two sequential forks from one parent get distinct branches");

            auto contribution_a = drive(sibling_a.history_provider().on_context(call.session_ctx, call.ctx));
            auto args_a = json::Value::make_object(
                {{"command", json::Value::make_string("echo -n 'sibling A only' > sibling_a_only.txt")}});
            check(contribution_a.has_value() && !contribution_a->tools.empty() &&
                      contribution_a->tools[0].invoke(args_a, call.ctx).has_value(),
                  "sibling_a's own real command succeeds");

            auto contribution_b = drive(sibling_b.history_provider().on_context(call.session_ctx, call.ctx));
            auto args_b = json::Value::make_object(
                {{"command", json::Value::make_string("echo -n 'sibling B only' > sibling_b_only.txt")}});
            check(contribution_b.has_value() && !contribution_b->tools.empty() &&
                      contribution_b->tools[0].invoke(args_b, call.ctx).has_value(),
                  "sibling_b's own real command succeeds");

            check(!read_entry(ledger, branch_a, owner, "sibling_b_only.txt").has_value(),
                  "sibling_b's file is invisible on sibling_a's branch");
            check(!read_entry(ledger, branch_b, owner, "sibling_a_only.txt").has_value(),
                  "sibling_a's file is invisible on sibling_b's branch");
            auto a_only = read_entry(ledger, branch_a, owner, "sibling_a_only.txt");
            check(a_only.has_value() && *a_only == "sibling A only", "sibling_a's own file is correct");
            auto b_only = read_entry(ledger, branch_b, owner, "sibling_b_only.txt");
            check(b_only.has_value() && *b_only == "sibling B only", "sibling_b's own file is correct");
        }
    }

    // [5] clear_in_process_state() relinquishes the branch and leaves the session re-bindable.
    {
        Session child;
        child.initialize("child-to-clear", Principal{"child-owner", ""});
        child.fork_from(parent, "child-to-clear-session");
        check(child.history_provider().is_bound(), "child forked from parent starts bound");
        if (child.history_provider().is_bound()) {
            std::string const old_branch_name = child.history_provider().runtime()->branch_name();
            child.clear_in_process_state();
            check(!child.history_provider().is_bound(), "clear_in_process_state() leaves it unbound");
            check(child.session_id().empty(), "clear_in_process_state() also clears session_id_");

            auto new_root = drive(ledger.create_root_branch(owner, "child-reused"));
            check(new_root.has_value(), "create_root_branch(owner, \"child-reused\") succeeds");
            if (new_root.has_value()) {
                child.history_provider().bind_sandbox(ledger, std::move(*new_root), owner,
                                                          scratch_root / "child-reused", branch_quota,
                                                          run_quota, storage_quota);
                check(child.history_provider().is_bound(), "the cleared session re-binds successfully");
                check(child.history_provider().runtime()->branch_name() != old_branch_name,
                      "the re-bound branch is genuinely new, not the relinquished one");
            }
        }
    }

    // [6] would_fork_succeed() reflects real, live BranchCost quota exhaustion.
    {
        auto tiny_branch_quota_r = agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 1);
        check(tiny_branch_quota_r.has_value(), "mint_root(BranchCost, 1) succeeds");
        if (tiny_branch_quota_r.has_value()) {
            Session temp_parent;
            temp_parent.initialize("temp-parent-would-fork", Principal{"temp-parent-owner", ""});
            auto temp_root = drive(ledger.create_root_branch(owner, "temp-parent-would-fork"));
            check(temp_root.has_value(), "create_root_branch(owner, \"temp-parent-would-fork\") succeeds");
            if (temp_root.has_value()) {
                temp_parent.history_provider().bind_sandbox(ledger, std::move(*temp_root), owner,
                                                                 scratch_root / "temp-parent",
                                                                 *tiny_branch_quota_r, run_quota,
                                                                 storage_quota);
                check(temp_parent.history_provider().would_fork_succeed().has_value(),
                      "would_fork_succeed() reports success while quota remains");

                Session temp_child;
                temp_child.initialize("temp-child-would-fork", Principal{"temp-child-owner", ""});
                temp_child.fork_from(temp_parent, "temp-child-would-fork-session");
                check(temp_child.history_provider().is_bound(),
                      "the real fork_from() that spends the last unit of quota still succeeds");

                auto exhausted = temp_parent.history_provider().would_fork_succeed();
                check(!exhausted.has_value(),
                      "would_fork_succeed() reports failure once quota is genuinely exhausted");
                if (!exhausted.has_value()) {
                    check(exhausted.error().code == "mandatory_sandbox_provider.branch_quota_exhausted",
                          "the failure carries mandatory_sandbox_provider.branch_quota_exhausted");
                }
            }
        }
    }

    // [7] ADR-119's new static Capabilities<cap::decl::RunCommand> ceiling fails closed through the
    // REAL pipeline: bind_sandbox() alone (the identity/quota gate, satisfied), but the session's own
    // granted CapabilitySet is explicitly EMPTY -- proving the ceiling is load-bearing, not merely
    // declared and never actually checked. Mirrors ADR-117 §7's identical proof shape for the
    // task-branch tools.
    {
        Session ungranted;
        ungranted.initialize("ungranted-session", real_owner_principal);
        auto root_r = drive(ledger.create_root_branch(owner, "ungranted"));
        check(root_r.has_value(), "create_root_branch(owner, \"ungranted\") succeeds");
        if (root_r.has_value()) {
            ungranted.history_provider().bind_sandbox(ledger, std::move(*root_r), owner,
                                                        scratch_root / "ungranted", branch_quota,
                                                        run_quota, storage_quota);
            check(ungranted.history_provider().is_bound(),
                  "bind_sandbox() alone leaves the provider bound (the identity/quota gate)");

            ScriptedChatClient& client = ungranted.emplace_chat_client();
            std::string const args_json = json::dump(json::Value::make_object(
                {{"command", json::Value::make_string(
                                 "echo -n 'should never run' > no_capability.txt")}}));
            client.set_script({tool_call_message("call-1", "run_command", args_json)});

            CapabilitySet const no_caps = CapabilitySet::grant_root({});
            ungranted.set_capabilities(&no_caps);

            std::uint64_t const run_quota_before = run_quota.remaining();
            auto response = drive(ungranted.start_run(agentengine::rt::StartRun{user_message("go")}));
            check(response.has_value(),
                  "start_run() itself still completes -- invoke_tool()'s rejection is a normal "
                  "role::tool error result, not a thrown/aborted run");

            bool found_capability_error = false;
            bool found_real_reply = false;
            for (Message const& m : ungranted.history()) {
                if (m.role != role::tool) continue;
                for (ContentItem const& item : m.content) {
                    auto const* tr = std::get_if<ToolResult>(&item.value);
                    if (!tr) continue;
                    if (tr->is_error) found_capability_error = true;
                }
                if (tool_reply_json_of(m).has_value()) found_real_reply = true;
            }
            check(found_capability_error,
                  "run_command is rejected as a real role::tool error result when the session holds no "
                  "cap::RunCommand grant, even though bind_sandbox() alone was called");
            check(!found_real_reply, "no real run_command call ever ran -- no real reply anywhere in history");
            check(run_quota.remaining() == run_quota_before,
                  "a rejected-at-authorization call never reaches SandboxRuntime::run() at all, so it "
                  "spends no RunCost unit -- the ceiling check runs strictly before the real verb");

            auto leaked = read_entry(ledger, ungranted.history_provider().runtime()->branch_name(), owner,
                                       "no_capability.txt");
            check(!leaked.has_value(), "the command never ran, so no_capability.txt was never written");
        }
    }

    std::filesystem::remove_all(scratch_root, ec);

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- MandatorySandboxProvider composes as the REAL, production "
                     "agentengine::rt::AgentSession's actual HistoryProviderT, including -- for the "
                     "first time in this design's entire history -- a run_command tool call driven end "
                     "to end through the real, unmodified session.start_run() -> invoke_tool() 10-step "
                     "pipeline, and ADR-119's real cap::RunCommand capability ceiling fail-closed proof, "
                     "against a REAL Docker daemon.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
