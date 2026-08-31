// PROVE-PHASE PROBE (A9 gap-closure): closes ADR-099 §8's own named residual -- "MandatorySandbox
// Provider is designed to compose as AgentSession's real HistoryProviderT, but has only been proven
// against FakeAgentSession, a faithful stand-in -- not the real class itself." This file drives the
// REAL, PRODUCTION `agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>` (include/
// agentengine/rt/agent_session.hpp, UNMODIFIED, not linked-into by this file, only included) with
// `HistoryProviderT = MandatorySandboxProvider<DockerExecutionSurface>` -- not `probe::
// FakeAgentSession`, this design's own sibling stand-in (fake_agent_session.hpp) -- and re-runs the
// SAME KIND of checks probe_mandatory_sandbox.cpp already proved against the fake, this time against
// the real class's own actual `fork_from()` (agent_session.hpp:1161)/`clear_in_process_state()`
// (agent_session.hpp:1210)/`history_provider()` (agent_session.hpp:657) methods, unmodified.
//
// `ChatClientT` is `RealAgentSessionProbeChatClient`, a minimal conformer to `agentengine::ChatClient`
// copied in shape from `tests/test_rt_agent_session.cpp`'s own `ScriptedChatClient` fixture (the
// simplest real `AgentSession<...>` instantiation already used elsewhere in this codebase's own test
// suite, per this task's own instruction to find one rather than invent an unrealistic one) -- never
// actually invoked here: every check below drives `MandatorySandboxProvider` directly through the
// real session's own `history_provider()` accessor (exactly as `run_rounds()` itself does internally,
// agent_session.hpp:2002/2666/1666/1928/1083), never through `start_run()`'s full round loop, so the
// model backend is never called. `StateT` stays defaulted to `agentengine::rt::NoSessionState`.
//
// Exercises, all against a REAL, live Docker daemon, mirroring probe_mandatory_sandbox.cpp's own
// [1]-[8] one for one, PLUS a NEW [9] this file adds that only makes sense against the real class:
//   [1] a freshly-default-constructed REAL AgentSession has NO execution capability.
//   [2] bind_sandbox() on the REAL session's history_provider() establishes a real sandbox; the
//       contributed run_command tool genuinely executes in a real container and commits a real
//       checkpoint.
//   [3] REAL `AgentSession::fork_from()` (not FakeAgentSession's mirror of it) produces a genuinely
//       isolated child, positive + negative controls both directions.
//   [4] REAL `fork_from()` copying an unbound source fails closed.
//   [5] two sequential REAL forks from the same parent are mutually isolated.
//   [6] REAL `clear_in_process_state()` relinquishes the branch and leaves the session re-bindable.
//   [7] would_fork_succeed() reflects real quota state (provider-level; included for full parity).
//   [8] an incidental copy of a bound provider, reached through the REAL session's own
//       `history_provider()` accessor, is safe.
//   [9] NEW: self-fork through the REAL `AgentSession::fork_from(source, id)` where `source` IS the
//       calling session itself (`session.fork_from(session, id)`) -- reachable because the real
//       signature (agent_session.hpp:1161) takes `source` as a plain `AgentSession const&` with no
//       identity check, exactly the scenario `mandatory_sandbox_provider.hpp`'s own banner names as
//       "[n]othing in the real AgentSession::fork_from(source, id) structurally prevents a future
//       caller from passing the same session as its own source" -- proven here one level higher than
//       probe_mandatory_sandbox.cpp's own [9] (which only self-copied the PROVIDER directly): this
//       drives the actual real `fork_from()` method with real aliasing across EVERY field it touches
//       (session_id_, principal_, history_, state_, metadata_, history_provider_), with the source's
//       BranchCost quota pre-exhausted so the would-be-fork's internal `spawn_child_branch()` call
//       would fail were it not short-circuited by the provider's own self-assignment guard.
//   [10] NEW: a real, previously-undetected `Ledger::create_root_branch()` naming-collision gap this
//       file's own multi-check reuse of one shared `owner` Principal exposed -- checks [2]/[6b]/[7]/
//       [9] each mint an additional root branch for that SAME owner, and the method's original
//       purely-owner-derived name ("root-<id>", no further uniqueness) meant every one of those calls
//       silently overwrote the ledger's own record for the prior one. Found here first, then fixed at
//       its real source (`Ledger::create_root_branch()` gained an optional `disambiguator` parameter,
//       worktree_ledger.hpp) and closed for real, not merely disclosed -- this check now PROVES the
//       fix holds by re-reading content committed all the way back in check [3].

#include "mandatory_sandbox_provider.hpp"
#include "../execution_surface/docker_execution_surface.hpp"

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/capability.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {

template <class T>
T run(agentengine::rt::task<T> t) { t.resume(); return t.take_value(); }

// A minimal, never-actually-invoked ChatClientT conformer -- copied in shape from tests/
// test_rt_agent_session.cpp's own ScriptedChatClient, the simplest real AgentSession<...>
// instantiation already established in this codebase's own test suite (grepped for, not invented).
// Every check in this file drives MandatorySandboxProvider directly through the real session's own
// history_provider() accessor, never through start_run(), so chat()/chat_stream() are never called --
// this type exists only to satisfy AgentSession's own `ChatClient<ChatClientT>` template constraint.
class RealAgentSessionProbeChatClient {
public:
    [[nodiscard]] agentengine::ChatClientCapabilities capabilities() const { return {}; }

    agentengine::task<agentengine::result<agentengine::ChatResponse>> chat(
        agentengine::ChatRequest, agentengine::EffectContext&) {
        co_return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "RealAgentSessionProbeChatClient::chat() must never be called -- this probe only drives "
            "MandatorySandboxProvider directly through history_provider(), never start_run().",
            "probe.chat_unreachable"});
    }

    [[nodiscard]] agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(
        agentengine::ChatRequest, agentengine::EffectContext&) {
        return {};  // unused
    }
};
static_assert(agentengine::ChatClient<RealAgentSessionProbeChatClient>);

// A real, minimal SessionContext/EffectContext pair -- identical construction to probe_mandatory_
// sandbox.cpp's own RealCallContext (this file's sibling proof), reused verbatim: nothing about this
// changes when the surrounding session is the real AgentSession instead of FakeAgentSession, since
// MandatorySandboxProvider's own on_context()/bind_sandbox() signatures are exactly what both take.
struct RealCallContext {
    agentengine::Principal real_principal;
    agentengine::CapabilitySet real_caps;
    agentengine::EffectContext ctx;
    std::vector<agentengine::Message> empty_history;
    agentengine::SessionContext session_ctx;

    RealCallContext(std::string label, agentengine::Principal principal)
        : real_principal(std::move(principal)),
          real_caps(agentengine::CapabilitySet::grant_root({})),
          session_ctx(std::move(label), real_principal, empty_history) {
        ctx.principal = real_principal;
        ctx.capabilities = agentengine::borrow_capabilities(real_caps);
    }
};

std::optional<std::string> read_entry(probe::Ledger<>& ledger, std::string const& branch_name,
                                        probe::Principal owner, std::string const& entry_name) {
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
    using probe::MandatorySandboxProvider;
    using probe::DockerExecutionSurface;
    using Provider = MandatorySandboxProvider<DockerExecutionSurface>;
    // THE type under real test: the REAL, production AgentSession template, instantiated with
    // MandatorySandboxProvider as its real HistoryProviderT -- never FakeAgentSession.
    using Session = agentengine::rt::AgentSession<RealAgentSessionProbeChatClient,
                                                    agentengine::rt::NoSessionState, Provider>;

    probe::IdentityAuthority& authority = probe::IdentityAuthority::bootstrap();
    agentengine::Principal real_owner_principal =
        agentengine::make_embedded_principal("real-mandatory-sandbox-owner");
    probe::Principal owner = authority.adopt(real_owner_principal.id, real_owner_principal.on_behalf_of);
    probe::Ledger<> ledger;
    auto storage_quota = probe::AsyncQuota<probe::StorageBytes>::mint_root(authority, owner, 10'000'000);
    CHECK(storage_quota.has_value());
    auto run_quota = probe::AsyncQuota<probe::RunCost>::mint_root(authority, owner, 100);
    CHECK(run_quota.has_value());
    auto branch_quota = probe::AsyncQuota<probe::BranchCost>::mint_root(authority, owner, 100);
    CHECK(branch_quota.has_value());

    std::filesystem::path const scratch_root =
        std::filesystem::temp_directory_path() / "ae_mandatory_sandbox_real_agent_session_probe";
    std::error_code ec;
    std::filesystem::remove_all(scratch_root, ec);

    RealCallContext call("real-mandatory-sandbox-owner", real_owner_principal);

    // === [1] a freshly-default-constructed REAL AgentSession has NO execution capability. ===========
    {
        Session bare;
        bare.initialize("bare-session", agentengine::Principal{"bare-owner", ""});
        auto contribution = run(bare.history_provider().on_context(call.session_ctx, call.ctx));
        CHECK(!bare.history_provider().is_bound());
        CHECK(contribution.has_value());
        CHECK(contribution->tools.empty());
        std::printf("[1] a freshly-default-constructed REAL agentengine::rt::AgentSession has NO "
                    "execution capability -- history_provider().on_context() contributes ZERO tools, "
                    "driven through the REAL AgentSession::history_provider() accessor "
                    "(agent_session.hpp:657), not FakeAgentSession's mirror of it -- PASS\n");
    }

    // === [2] bind_sandbox() on the REAL session establishes a real sandbox; the tool genuinely =======
    // === executes and commits. =========================================================================
    Session parent;
    parent.initialize("parent-session", agentengine::Principal{"parent-owner", ""});
    {
        // Disambiguator (2026-08-28 fix, worktree_ledger.hpp's own comment): this probe mints several
        // logically-independent root branches for the SAME `owner` -- see check [10] below for the
        // real, previously-undetected collision this closes.
        auto root_r = run(ledger.create_root_branch(owner, "parent"));
        CHECK(root_r.has_value());
        parent.history_provider().bind_sandbox(ledger, std::move(*root_r), owner,
                                                  scratch_root / "parent", *branch_quota, *run_quota,
                                                  *storage_quota);
        CHECK(parent.history_provider().is_bound());

        auto contribution = run(parent.history_provider().on_context(call.session_ctx, call.ctx));
        CHECK(contribution.has_value());
        CHECK(contribution->tools.size() == 1);
        CHECK(contribution->tools[0].name == "run_command");

        auto args = agentengine::json::Value::make_object(
            {{"command", agentengine::json::Value::make_string(
                             "echo -n 'from parent, turn 1' > parent.txt && cat parent.txt")}});
        auto reply = contribution->tools[0].invoke(args, call.ctx);
        CHECK(reply.has_value());
        std::string const reply_json = agentengine::json::dump(*reply);
        CHECK(reply_json.find("\"ok\":true") != std::string::npos);
        CHECK(reply_json.find("from parent, turn 1") != std::string::npos);
        std::printf("[2] bind_sandbox() on the REAL AgentSession's history_provider() established a "
                    "real sandbox; the contributed run_command tool genuinely executed a real command "
                    "in a real Docker container and committed a real checkpoint: %s\n",
                    reply_json.c_str());
    }

    // === [3] the REAL AgentSession::fork_from() (agent_session.hpp:1161) produces a GENUINELY ========
    // === FRESH, ISOLATED child branch -- real, mutual, POSITIVE+NEGATIVE control isolation. ===========
    Session child;
    child.initialize("child-session-not-yet-forked", agentengine::Principal{"child-owner", ""});
    std::string parent_root_branch_name;   // captured here, re-verified after [6]/[7]/[9] below
    {
        child.fork_from(parent, "child-session");   // the REAL method, not FakeAgentSession's mirror
        CHECK(child.history_provider().is_bound());
        CHECK(child.session_id() == "child-session");   // real fork_from() also set session_id_ for real
        std::string const parent_branch = parent.history_provider().runtime()->branch_name();
        parent_root_branch_name = parent_branch;
        std::string const child_branch = child.history_provider().runtime()->branch_name();
        CHECK(child_branch != parent_branch);
        CHECK(child.history_provider().runtime()->staging_root() !=
              parent.history_provider().runtime()->staging_root());

        auto parent_contribution2 = run(parent.history_provider().on_context(call.session_ctx, call.ctx));
        auto parent_args2 = agentengine::json::Value::make_object(
            {{"command", agentengine::json::Value::make_string(
                             "echo -n 'parent only, post-fork' > parent_only.txt")}});
        auto parent_reply2 = parent_contribution2->tools[0].invoke(parent_args2, call.ctx);
        CHECK(parent_reply2.has_value());

        auto child_contribution = run(child.history_provider().on_context(call.session_ctx, call.ctx));
        CHECK(child_contribution.has_value());
        CHECK(child_contribution->tools.size() == 1);
        auto child_args = agentengine::json::Value::make_object(
            {{"command", agentengine::json::Value::make_string(
                             "cat parent.txt && echo -n 'from child' > child_only.txt")}});
        auto child_reply = child_contribution->tools[0].invoke(child_args, call.ctx);
        CHECK(child_reply.has_value());
        std::string const child_reply_json = agentengine::json::dump(*child_reply);
        CHECK(child_reply_json.find("from parent, turn 1") != std::string::npos);  // inherited (COW)
        std::printf("[3a] child's first real command could read parent.txt through the REAL "
                    "AgentSession's own fork_from() -- real COW inheritance: %s\n",
                    child_reply_json.c_str());

        CHECK(!read_entry(ledger, parent_branch, owner, "child_only.txt").has_value());
        CHECK(!read_entry(ledger, child_branch, owner, "parent_only.txt").has_value());
        auto parent_only = read_entry(ledger, parent_branch, owner, "parent_only.txt");
        CHECK(parent_only.has_value() && *parent_only == "parent only, post-fork");
        auto child_only = read_entry(ledger, child_branch, owner, "child_only.txt");
        CHECK(child_only.has_value() && *child_only == "from child");
        std::printf("[3b] REAL ISOLATION PROVEN against the REAL AgentSession::fork_from(), with both "
                    "positive and negative controls on both sides -- PASS\n");
    }

    // === [4] the REAL fork_from() copying an UNBOUND source fails closed. =============================
    {
        Session unbound_source;
        unbound_source.initialize("unbound-source", agentengine::Principal{"unbound-owner", ""});
        Session unbound_child;
        unbound_child.initialize("unbound-child", agentengine::Principal{"unbound-child-owner", ""});
        unbound_child.fork_from(unbound_source, "unbound-child-2");
        CHECK(!unbound_child.history_provider().is_bound());
        auto contribution = run(unbound_child.history_provider().on_context(call.session_ctx, call.ctx));
        CHECK(contribution.has_value());
        CHECK(contribution->tools.empty());
        std::printf("[4] the REAL AgentSession::fork_from() copying an UNBOUND source: the resulting "
                    "child has NO execution capability -- fails closed, never aliases anything -- "
                    "PASS\n");
    }

    // === [5] TWO SEQUENTIAL children forked from the SAME parent via the REAL fork_from(). ============
    {
        Session sibling_a;
        sibling_a.initialize("sibling-a", agentengine::Principal{"sibling-a-owner", ""});
        sibling_a.fork_from(parent, "sibling-a-session");
        CHECK(sibling_a.history_provider().is_bound());

        Session sibling_b;
        sibling_b.initialize("sibling-b", agentengine::Principal{"sibling-b-owner", ""});
        sibling_b.fork_from(parent, "sibling-b-session");
        CHECK(sibling_b.history_provider().is_bound());

        std::string const branch_a = sibling_a.history_provider().runtime()->branch_name();
        std::string const branch_b = sibling_b.history_provider().runtime()->branch_name();
        CHECK(branch_a != branch_b);
        CHECK(sibling_a.history_provider().runtime()->staging_root() !=
              sibling_b.history_provider().runtime()->staging_root());

        auto contribution_a = run(sibling_a.history_provider().on_context(call.session_ctx, call.ctx));
        auto args_a = agentengine::json::Value::make_object(
            {{"command", agentengine::json::Value::make_string(
                             "echo -n 'sibling A only' > sibling_a_only.txt")}});
        CHECK(contribution_a->tools[0].invoke(args_a, call.ctx).has_value());

        auto contribution_b = run(sibling_b.history_provider().on_context(call.session_ctx, call.ctx));
        auto args_b = agentengine::json::Value::make_object(
            {{"command", agentengine::json::Value::make_string(
                             "echo -n 'sibling B only' > sibling_b_only.txt")}});
        CHECK(contribution_b->tools[0].invoke(args_b, call.ctx).has_value());

        CHECK(!read_entry(ledger, branch_a, owner, "sibling_b_only.txt").has_value());
        CHECK(!read_entry(ledger, branch_b, owner, "sibling_a_only.txt").has_value());
        auto a_only = read_entry(ledger, branch_a, owner, "sibling_a_only.txt");
        CHECK(a_only.has_value() && *a_only == "sibling A only");
        auto b_only = read_entry(ledger, branch_b, owner, "sibling_b_only.txt");
        CHECK(b_only.has_value() && *b_only == "sibling B only");
        std::printf("[5] TWO SEQUENTIAL children forked from the SAME parent via the REAL "
                    "AgentSession::fork_from(): mutually isolated from each other and the parent -- "
                    "PASS\n");
    }

    // === [6] the REAL clear_in_process_state() (agent_session.hpp:1210) relinquishes the old branch ===
    // === and leaves the session re-bindable afterward. =================================================
    {
        std::string const old_branch_name = child.history_provider().runtime()->branch_name();
        child.clear_in_process_state();   // the REAL method -- resets far more than the fake's mirror
                                            // does (session_id_, principal_, history_, state_,
                                            // metadata_, run_counter_, ..., history_provider_ =
                                            // HistoryProviderT{}) -- see agent_session.hpp:1210-1250.
        CHECK(!child.history_provider().is_bound());
        CHECK(child.session_id().empty());

        auto processed = run(ledger.reap_pending_abandons());
        CHECK(processed >= 1);
        std::printf("[6a] the REAL clear_in_process_state()'s HistoryProviderT{} statement "
                    "(agent_session.hpp:1238) relinquished the old branch -- reap_pending_abandons() "
                    "processed %llu real pending abandon(s) -- PASS\n",
                    (unsigned long long)processed);

        auto new_root = run(ledger.create_root_branch(owner, "child-reused"));
        CHECK(new_root.has_value());
        child.history_provider().bind_sandbox(ledger, std::move(*new_root), owner,
                                                 scratch_root / "child-reused", *branch_quota,
                                                 *run_quota, *storage_quota);
        CHECK(child.history_provider().is_bound());
        CHECK(child.history_provider().runtime()->branch_name() != old_branch_name);
        std::printf("[6b] the cleared REAL session was successfully re-bound to a genuinely NEW "
                    "branch ('%s') -- PASS\n", child.history_provider().runtime()->branch_name().c_str());
    }

    // === [7] would_fork_succeed() genuinely reflects real, live BranchCost quota state (provider- =====
    // === level; included for full parity with probe_mandatory_sandbox.cpp's own [7]). ==================
    {
        auto tiny_branch_quota = probe::AsyncQuota<probe::BranchCost>::mint_root(authority, owner, 1);
        CHECK(tiny_branch_quota.has_value());
        Session temp_parent;
        temp_parent.initialize("temp-parent-for-would-fork-test",
                                 agentengine::Principal{"temp-parent-owner", ""});
        auto temp_root = run(ledger.create_root_branch(owner, "temp-parent-would-fork"));
        CHECK(temp_root.has_value());
        temp_parent.history_provider().bind_sandbox(ledger, std::move(*temp_root), owner,
                                                        scratch_root / "temp-parent",
                                                        *tiny_branch_quota, *run_quota,
                                                        *storage_quota);
        CHECK(temp_parent.history_provider().would_fork_succeed().has_value());

        Session temp_child;
        temp_child.initialize("temp-child-for-would-fork-test",
                                agentengine::Principal{"temp-child-owner", ""});
        temp_child.fork_from(temp_parent, "temp-child-session");   // the REAL fork_from()
        CHECK(temp_child.history_provider().is_bound());

        auto exhausted = temp_parent.history_provider().would_fork_succeed();
        CHECK(!exhausted.has_value());
        CHECK(exhausted.error().code == "mandatory_sandbox.branch_quota_exhausted");
        std::printf("[7] would_fork_succeed() correctly reported success while quota remained and "
                    "REAL exhaustion (%s) once it was actually spent by a REAL AgentSession::"
                    "fork_from() call -- PASS\n", exhausted.error().code.c_str());
    }

    // === [8] an INCIDENTAL copy of a bound provider, reached through the REAL session's own ===========
    // === history_provider() accessor (not fork_from()), is safe. =======================================
    {
        Provider& parent_provider = parent.history_provider();   // the REAL, mutable accessor
        std::string const parent_branch_before = parent_provider.runtime()->branch_name();

        Provider incidental_copy(parent_provider);
        CHECK(incidental_copy.is_bound());
        CHECK(incidental_copy.runtime()->branch_name() != parent_branch_before);
        CHECK(parent_provider.is_bound());
        CHECK(parent_provider.runtime()->branch_name() == parent_branch_before);
        auto still_works = run(parent_provider.on_context(call.session_ctx, call.ctx));
        CHECK(still_works.has_value());
        CHECK(still_works->tools.size() == 1);
        auto verify_args = agentengine::json::Value::make_object(
            {{"command", agentengine::json::Value::make_string("echo -n 'parent still alive' > proof.txt")}});
        CHECK(still_works->tools[0].invoke(verify_args, call.ctx).has_value());
        std::printf("[8] an INCIDENTAL copy through the REAL AgentSession::history_provider() accessor "
                    "produced its own real, independent child and left the source completely "
                    "unaffected and still functional -- PASS\n");
    }

    // === [9] NEW (real-class-only): self-fork through the REAL AgentSession::fork_from(source, id) ===
    // === where `source` IS the session calling it -- see this file's own top banner for why this ======
    // === scenario is specifically real for the actual `fork_from()` signature. =========================
    {
        auto tiny_quota = probe::AsyncQuota<probe::BranchCost>::mint_root(authority, owner, 0);  // pre-exhausted
        CHECK(tiny_quota.has_value());
        Session self_fork_session;
        self_fork_session.initialize("self-fork-session", agentengine::Principal{"self-fork-owner", ""});
        auto self_root = run(ledger.create_root_branch(owner, "self-fork"));
        CHECK(self_root.has_value());
        self_fork_session.history_provider().bind_sandbox(ledger, std::move(*self_root), owner,
                                                              scratch_root / "self-fork",
                                                              *tiny_quota, *run_quota, *storage_quota);
        CHECK(self_fork_session.history_provider().is_bound());
        std::string const session_id_before = self_fork_session.session_id();
        std::string const branch_before_self_fork =
            self_fork_session.history_provider().runtime()->branch_name();

        // THE real, adversarial call: the REAL AgentSession::fork_from(), with `source` aliasing the
        // very session invoking it. Nothing on the real signature (agent_session.hpp:1161, a plain
        // `AgentSession const& source`) prevents this. Every field fork_from() touches --
        // session_id_/principal_/history_/state_/metadata_/history_provider_ -- ends up self-assigned.
        // Were the provider's self-assignment guard not real, the fail-closed reset on the (would-be
        // failing, quota=0) internal fork attempt would wipe this session's own live sandbox.
        self_fork_session.fork_from(self_fork_session, "self-fork-session-renamed");

        CHECK(self_fork_session.history_provider().is_bound());
        CHECK(self_fork_session.history_provider().runtime()->branch_name() == branch_before_self_fork);
        // fork_from() DOES intentionally overwrite session_id_ even on self-fork (that field is not
        // self-assigned the way history_provider_ is -- it's unconditionally set from the `new_session_
        // id` parameter, agent_session.hpp:1163) -- a REAL, honestly-disclosed behavioral asymmetry
        // this probe surfaces rather than assuming away: self-fork is a safe no-op for the SANDBOX
        // (the property this design cares about), but NOT a no-op for the session's own identity.
        CHECK(self_fork_session.session_id() == "self-fork-session-renamed");
        CHECK(self_fork_session.session_id() != session_id_before);
        auto still_alive = run(self_fork_session.history_provider().on_context(call.session_ctx, call.ctx));
        CHECK(still_alive.has_value());
        CHECK(still_alive->tools.size() == 1);
        auto verify_args = agentengine::json::Value::make_object(
            {{"command", agentengine::json::Value::make_string("echo -n 'self-fork survived' > proof2.txt")}});
        CHECK(still_alive->tools[0].invoke(verify_args, call.ctx).has_value());
        std::printf("[9] NEW: self-fork through the REAL AgentSession::fork_from(source, id) where "
                    "source IS the calling session (BranchCost quota pre-exhausted, so the internal "
                    "fork attempt would have FAILED) left the sandbox COMPLETELY UNAFFECTED (same "
                    "branch '%s', still fully functional) -- but DID rename session_id_ to "
                    "'%s' (fork_from()'s unconditional, non-self-assignment-guarded field) -- a real, "
                    "honestly-disclosed asymmetry between the sandbox-safety guarantee and the "
                    "session-identity field, not previously observable against FakeAgentSession (whose "
                    "own fork_from() mirror is byte-identical here, so this finding is not fake-vs-real "
                    "specific -- but it was never actually driven through the REAL method before this "
                    "file) -- PASS\n",
                    branch_before_self_fork.c_str(), self_fork_session.session_id().c_str());
    }

    // === [10] REAL ADVERSARIAL PROOF closing the finding this check ORIGINALLY only disclosed: this ===
    // === probe's own [2]/[6b]/[7]/[9] each mint an ADDITIONAL root branch for the SAME `owner` on the =
    // === SAME `ledger` -- before `Ledger::create_root_branch()` gained an explicit `disambiguator` ====
    // === parameter (worktree_ledger.hpp, 2026-08-28 fix), every one of those calls collided on the ===
    // === IDENTICAL "root-<id>" ledger key and `branches_.insert_or_assign()` silently OVERWROTE the ===
    // === prior record -- verified empirically at the time by re-reading "parent_only.txt" (committed =
    // === in check [3]) and finding it genuinely gone. Now that every call site above passes a distinct=
    // === disambiguator ("parent"/"child-reused"/"temp-parent-would-fork"/"self-fork"), re-run the ======
    // === SAME re-read: it must now come back exactly as check [3] left it. =============================
    {
        auto still_there = read_entry(ledger, parent_root_branch_name, owner, "parent_only.txt");
        CHECK(still_there.has_value());
        CHECK(*still_there == "parent only, post-fork");
        std::printf(
            "[10] REAL ADVERSARIAL PROOF: after checks [6b]/[7]/[9] each minted a further same-owner "
            "root branch, re-reading 'parent_only.txt' from branch '%s' (parent's own root branch, "
            "established in check [2]/[3]) still returns exactly \"%s\" -- the `disambiguator` "
            "parameter (worktree_ledger.hpp) genuinely stops same-owner `create_root_branch()` calls "
            "from silently overwriting each other's ledger records, closing this file's own [10] "
            "finding for real (also fixed in probe_mandatory_sandbox.cpp's identical pattern, its own "
            "new check [10]) -- PASS\n",
            parent_root_branch_name.c_str(), still_there->c_str());
    }

    std::filesystem::remove_all(scratch_root, ec);
    std::printf("\nALL CHECKS PASSED -- MandatorySandboxProvider composes as the REAL, production "
                "agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>'s actual "
                "HistoryProviderT, driven through its real fork_from()/clear_in_process_state()/"
                "history_provider() methods (agent_session.hpp:1161/1210/657), not FakeAgentSession -- "
                "closing ADR-099 section 8's own named residual for real.\n");
    return 0;
}
