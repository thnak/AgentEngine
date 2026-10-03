// ADR-209 build step 5 (GitHub issue #146): `LiveShellSandboxProvider` -- the `shell_exec` tool, lifetimes,
// ceilings, the open budget, snapshots and the provider's reset mapping -- driven through the real tool
// descriptor against an in-test `PersistentShellSurface`. NO daemon (the live-Docker half is C1-C17, step 8).
//
//   P1  unbound: no tool; `bind_sandbox` refuses a zero ceiling (`live_shell.limits_required`) and stays unbound.
//   P2  the first shell_exec opens and commits; the next one at the same head does not re-open; cwd persists.
//   P3  lifetime::PerRun: on_run_end releases the environment; the next shell_exec re-opens and replays the cwd
//       the last command left (the snapshot keyed by the head checkpoint).
//   P4  lifetime::PerSession: on_run_end keeps it -- until `max_runs` completed runs, the engine's ceiling.
//   P5  `max_idle` (checked by on_context) and `max_alive` (checked by shell_exec) release the environment
//       whatever the policy says.
//   P6  `restart` re-opens and is charged to LiveShellOpen; once that budget is spent a restart is refused with
//       a named error, nothing runs and RunCost is refunded (C10's "uncharged restarts" control).
//   P7  the provider's reset_to_turn(n) re-opens with turn n's files AND cwd -- including when turn n's tree
//       equals the head's (C5's two plants).
//   P8  a lost shell re-opens with the LAST COMPLETED command's cwd (the provider maps the new checkpoint to
//       the prior head's snapshot).
//   P9  a commit failing after the command ran is a reply (ok=false, commit_error, output intact) and the
//       next command reports the discard (C17).
//   P10 a principal that is not the quota owner is refused before anything runs (C6).
//   P11 copy (fork): a child branch, no live environment; the copy opens its own on first use and the
//       original's environment is untouched (I1).
//   P12 a failed release is reported (a `sandbox_exec_finished` "release" event, ok=false), the environment is
//       kept, and the next check point retries it.
//   P13 the destructor releases a held environment.
//   P14 composed: inside ComposedContextProvider the run-end hook still reaches the provider (C10).
//   P10b a delegate on_behalf_of the owner is refused in the owner's OPEN shell (C6: owner-only RunCost).
//   P15 C4: a real AgentSession run CANCELED between turns keeps every shell_exec that returned.
//   P16 C4: a run whose next model call FAILS keeps the shell_exec before it.
//   P17 (§15.5 M1) an environment opened for a command the surface then refuses is held, counted, audited and
//       released by the policy -- not invisible to every ceiling.
//   P18 (§15.5) a lost container whose removal failed stays held and the next check point retries it.
//
// Positive controls (planted by hand, recorded in ADR-209 §15): dropping check_lifetime() from on_run_end
// fails P3 and P14; an open hook that does not charge LiveShellOpen fails P6; dropping the lost-shell
// fallback mapping fails P8; dropping reset_to_turn()'s snapshot mapping fails P7; run_live() without the RunCost
// gate fails P10/P10b (C6); run_live() that "defers" its commit (never commits) fails P15/P16 (C4).

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/sandbox/live_shell_sandbox_provider.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace agentengine;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;
void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

struct Counters {
    int opens = 0;
    int execs = 0;
    int closes = 0;
    int close_failures_left = 0;
    int boxes = 0;
    std::vector<ShellSnapshot> replayed;
    fs::path root;
};

// The step-2 fake, with shared counters so a test can see every instance a factory made.
// Commands: "write <p> <c>" | "rm <p>" | "cd <d>" | "export K=V" | "noop" | "lose".
class FakeLiveSurface {
public:
    explicit FakeLiveSurface(std::shared_ptr<Counters> c)
        : c_(std::move(c)), box_(c_->root / ("box" + std::to_string(++c_->boxes))) {}

    [[nodiscard]] result<void> open(fs::path const& host_dir, ShellSnapshot const* snap) {
        ++c_->opens;
        std::error_code ec;
        fs::remove_all(box_, ec);
        fs::create_directories(box_, ec);
        fs::copy(host_dir, box_, fs::copy_options::recursive, ec);
        if (ec) return std::unexpected(error{failure_class::fatal, "seed failed: " + ec.message(), "test.seed"});
        cwd_ = "/workspace";
        env_.clear();
        if (snap != nullptr) {
            c_->replayed.push_back(*snap);
            if (!snap->cwd.empty()) cwd_ = snap->cwd;
            for (auto const& [k, v] : snap->env_set) env_[k] = v;
        }
        live_ = true;
        held_ = true;
        return {};
    }

    [[nodiscard]] result<LiveExecOutcome> exec(std::string const& command, std::chrono::milliseconds) {
        if (!live_) return std::unexpected(error{failure_class::contract, "no live shell", "test.not_live"});
        // What a real surface does with a NUL / oversized command: refused up front, nothing ran (§15.5 M1).
        if (command == "refuse") return std::unexpected(error{failure_class::policy, "refused", "test.refused"});
        if (command == "lose_container") {  // a timeout whose in-container kill did not take (§15.5 M2)
            live_ = false;
            LiveExecOutcome lost;
            lost.timed_out = true;
            lost.shell_lost = true;
            lost.container_lost = true;
            return lost;
        }
        ++c_->execs;
        LiveExecOutcome out;
        out.exit_code = 0;
        std::error_code ec;
        if (command.rfind("write ", 0) == 0) {
            auto const rest = command.substr(6);
            auto const sp = rest.find(' ');
            std::ofstream(box_ / rest.substr(0, sp), std::ios::binary) << rest.substr(sp + 1);
        } else if (command.rfind("rm ", 0) == 0) {
            fs::remove(box_ / command.substr(3), ec);
        } else if (command.rfind("cd ", 0) == 0) {
            cwd_ = command.substr(3);
        } else if (command.rfind("export ", 0) == 0) {
            auto const kv = command.substr(7);
            env_[kv.substr(0, kv.find('='))] = kv.substr(kv.find('=') + 1);
        } else if (command == "lose") {
            live_ = false;
            out.shell_lost = true;
            out.exit_code = -1;
            out.output = "partial";
            return out;
        }
        out.output = "ran: " + command + " in " + cwd_;
        out.output_bytes = out.output.size();
        ShellSnapshot snap;
        snap.cwd = cwd_;
        for (auto const& [k, v] : env_) snap.env_set.emplace_back(k, v);
        out.snapshot = snap;
        return out;
    }

    [[nodiscard]] result<void> drain_to(fs::path const& host_dir) {
        auto cleared = clear_directory_contents(host_dir, "test.drain_clear");
        if (!cleared.has_value()) return std::unexpected(cleared.error());
        std::error_code ec;
        fs::create_directories(host_dir, ec);
        fs::copy(box_, host_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) return std::unexpected(error{failure_class::fatal, "drain failed: " + ec.message(), "test.drain"});
        return {};
    }

    [[nodiscard]] result<void> close() {
        if (!held_) return {};
        if (c_->close_failures_left > 0) {
            --c_->close_failures_left;
            live_ = false;
            return std::unexpected(error{failure_class::fatal, "docker rm failed", "test.close_failed"});
        }
        ++c_->closes;
        live_ = false;
        held_ = false;
        return {};
    }
    [[nodiscard]] bool is_live() const { return live_; }

private:
    std::shared_ptr<Counters> c_;
    fs::path box_;
    bool live_ = false;
    bool held_ = false;
    std::string cwd_;
    std::map<std::string, std::string> env_;
};
static_assert(PersistentShellSurface<FakeLiveSurface>);

struct ManualClock {
    std::shared_ptr<std::chrono::steady_clock::time_point> now =
        std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::time_point{} + 1h);
    void advance(std::chrono::milliseconds d) const { *now += d; }
    [[nodiscard]] LiveShellClock fn() const {
        auto p = now;
        return [p] { return *p; };
    }
};

struct Recorded {
    std::vector<run_event_payload::SandboxExec> events;
};

[[nodiscard]] EffectContext ctx_for(std::string const& principal_id, Recorded* rec = nullptr) {
    EffectContext ctx;
    ctx.principal.id = principal_id;
    if (rec != nullptr) {
        ctx.sandbox_exec_sink = [rec](run_event_kind, run_event_payload::SandboxExec p) {
            rec->events.push_back(std::move(p));
        };
    }
    return ctx;
}

template <class Provider>
[[nodiscard]] std::optional<ToolDescriptor> shell_tool(Provider& p, EffectContext& ctx) {
    static std::vector<Message> const no_history;
    SessionContext sc{"live-shell-test", ctx.principal, no_history};
    auto contribution = drive(p.on_context(sc, ctx));
    if (!contribution.has_value()) return std::nullopt;
    for (auto& d : contribution->tools) {
        if (d.name == "shell_exec") return d;
    }
    return std::nullopt;
}

// Calls shell_exec through the real descriptor (schema parse included). Returns the reply or the error.
template <class Provider>
[[nodiscard]] result<ShellExecReply> shell(Provider& p, std::string const& command, std::string const& who = "owner",
                                           bool restart = false, std::string const& on_behalf_of = {}) {
    EffectContext ctx = ctx_for(who);
    ctx.principal.on_behalf_of = on_behalf_of;
    auto tool = shell_tool(p, ctx);
    if (!tool.has_value()) return std::unexpected(error{failure_class::contract, "no shell_exec tool", "test.no_tool"});
    std::vector<std::pair<std::string, json::Value>> members{{"command", json::Value::make_string(command)}};
    if (restart) members.emplace_back("restart", json::Value::make_bool(true));
    json::Value const args = json::Value::make_object(std::move(members));
    auto out = tool->invoke(args, ctx);
    if (!out.has_value()) return std::unexpected(out.error());
    return schema::from_json<ShellExecReply>(*out);
}

template <class Provider>
void run_end(Provider& p, Recorded* rec = nullptr) {
    EffectContext ctx = ctx_for("owner", rec);
    (void)drive(p.on_run_end(RunEndView{run_end_reason::final_answer}, ctx));
}

struct World {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.adopt(Principal{.id = "owner", .tenant_id = ""});
    Ledger<> ledger;
    agentengine::rt::AsyncQuota<BranchCost> branch_q = *agentengine::rt::AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    agentengine::rt::AsyncQuota<RunCost> run_q = *agentengine::rt::AsyncQuota<RunCost>::mint_root(authority, owner, 1000);
    agentengine::rt::AsyncQuota<StorageBytes> storage_q =
        *agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 100'000'000);
    agentengine::rt::AsyncQuota<LiveShellOpen> open_q =
        *agentengine::rt::AsyncQuota<LiveShellOpen>::mint_root(authority, owner, 1000);
    agentengine::rt::AsyncQuota<ResetCost> reset_q = *agentengine::rt::AsyncQuota<ResetCost>::mint_root(authority, owner, 100);
    std::shared_ptr<Counters> counters = std::make_shared<Counters>();
    ManualClock clock;
    fs::path dir;
    int seq = 0;

    explicit World(std::string const& name) : dir(fs::temp_directory_path() / ("ae_test_lssp_" + name)) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        counters->root = dir;
    }
    ~World() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    template <class Lifetime = lifetime::PerRun>
    [[nodiscard]] result<void> bind(LiveShellSandboxProvider<FakeLiveSurface, InMemoryWorktreeObjectStore, Lifetime>& p,
                                    LiveShellLimits limits = {1h, 10h, 100},
                                    agentengine::rt::AsyncQuota<LiveShellOpen>* open = nullptr,
                                    agentengine::rt::AsyncQuota<StorageBytes>* storage = nullptr) {
        auto root = drive(ledger.create_root_branch(owner, "b" + std::to_string(++seq)));
        if (!root.has_value()) return std::unexpected(root.error());
        auto c = counters;
        return p.bind_sandbox(ledger, std::move(*root), owner, dir / ("staging" + std::to_string(seq)), branch_q, run_q,
                              storage != nullptr ? *storage : storage_q, open != nullptr ? *open : open_q, limits,
                              [c] { return FakeLiveSurface(c); }, 10s, Lifetime{}, clock.fn());
    }
};

using PerRunProvider = LiveShellSandboxProvider<FakeLiveSurface>;
using PerSessionProvider = LiveShellSandboxProvider<FakeLiveSurface, InMemoryWorktreeObjectStore, lifetime::PerSession>;

[[nodiscard]] bool tree_has(Ledger<>& ledger, std::string const& tree_digest, IdentityHandle who, std::string const& name) {
    auto tree = ledger.get_tree_safe(tree_digest, who);
    if (!tree.has_value()) return false;
    return std::any_of(tree->entries.begin(), tree->entries.end(), [&](auto const& e) { return e.name == name; });
}

// A provider the plain AgentSession-shaped composition can hold next to it.
class PlainProvider {
public:
    static constexpr std::string_view name = "plain";
    task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) { co_return ContextContribution{}; }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};

// ---- C4: a real AgentSession, a scripted model. ---------------------------------------------------------------------
rt::AgentSessionCore* g_cancel_target = nullptr;
struct NoArgs {
    int unused = 0;
};
AE_JSON_SCHEMA(NoArgs, unused)
struct CancelTool : Tool<CancelTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "cancel_tool";
    static constexpr std::string_view description = "Cancels the run.";
    using Args = NoArgs;
    using Reply = NoArgs;
    static result<Reply> invoke(Args a, EffectContext&) {
        if (g_cancel_target != nullptr) g_cancel_target->cancel();
        return a;
    }
};
class CancelProvider {
public:
    static constexpr std::string_view name = "cancel_provider";
    task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ToolTable::from_tools<CancelTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};

struct Step {
    std::optional<Message> message;
    std::optional<error> failure;
};
class Client {
public:
    Client() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<Step> script;
        std::size_t calls = 0;
    };
    void set_script(std::vector<Step> s) { state_->script = std::move(s); }
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        std::size_t const idx = state_->calls < state_->script.size() ? state_->calls : state_->script.size() - 1;
        ++state_->calls;
        Step const& s = state_->script[idx];
        if (s.failure) co_return std::unexpected(*s.failure);
        co_return ChatResponse{*s.message, Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }

private:
    std::shared_ptr<State> state_;
};

[[nodiscard]] Message tool_calls(std::vector<std::pair<std::string, std::string>> calls) {
    Message m;
    m.role = role::assistant;
    int n = 0;
    for (auto& [tool, args] : calls) {
        ContentItem item;
        item.origin = content_origin::assistant;
        ToolCall c;
        c.call_id = "c" + std::to_string(++n);
        c.tool_name = tool;
        c.arguments_json = args;
        c.provenance = call_provenance::vendor_structured;
        item.value = c;
        m.content.push_back(item);
    }
    return m;
}
[[nodiscard]] Message user_text(std::string t) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(t)};
    m.content.push_back(item);
    return m;
}

using LiveComposed = ComposedContextProvider<PerRunProvider, CancelProvider>;
using LiveSession = rt::AgentSession<Client, rt::NoSessionState, LiveComposed>;

}  // namespace

int main() {
    static_assert(ContextProvider<PerRunProvider> && HasOnRunEnd<PerRunProvider>);

    // ---- P1
    {
        World w("p1");
        PerRunProvider p;
        EffectContext ctx = ctx_for("owner");
        check(!shell_tool(p, ctx).has_value(), "P1: an unbound provider contributes no tool");
        auto refused = w.bind(p, LiveShellLimits{0ms, 1h, 1});
        check(!refused.has_value() && refused.error().code == "live_shell.limits_required" && !p.is_bound(),
              "P1: a zero ceiling is refused and nothing is bound");
    }

    // ---- P2, P3
    {
        World w("p2");
        PerRunProvider p;
        check(w.bind(p).has_value(), "P2 setup: bind");
        auto a = shell(p, "write a.txt A");
        check(a.has_value() && a->ok && a->reopened && w.counters->opens == 1 &&
                  tree_has(w.ledger, a->tree_digest, w.owner, "a.txt"),
              "P2: the first shell_exec opens and commits before returning");
        (void)shell(p, "cd /workspace/sub");
        auto b = shell(p, "noop");
        check(b.has_value() && !b->reopened && w.counters->opens == 1 && b->output.find("in /workspace/sub") != std::string::npos,
              "P2: later commands at the same head reuse the shell and its cwd");

        run_end(p);
        check(w.counters->closes == 1 && !p.holds_environment(), "P3: PerRun releases at run end");
        auto c = shell(p, "noop");
        check(c.has_value() && c->reopened && w.counters->opens == 2 && !w.counters->replayed.empty() &&
                  w.counters->replayed.back().cwd == "/workspace/sub" && c->output.find("in /workspace/sub") != std::string::npos,
              "P3: the next run re-opens and replays the last command's cwd");
    }

    // ---- P4
    {
        World w("p4");
        PerSessionProvider p;
        check(w.bind(p, LiveShellLimits{1h, 10h, 2}).has_value(), "P4 setup: bind");
        (void)shell(p, "noop");
        run_end(p);
        check(w.counters->closes == 0 && p.holds_environment(), "P4: PerSession keeps the shell across a run end");
        (void)shell(p, "noop");
        check(w.counters->opens == 1, "P4: ... and the next run reuses it");
        run_end(p);
        check(w.counters->closes == 1 && !p.holds_environment(), "P4: max_runs (2) releases it whatever the policy says");
    }

    // ---- P5
    {
        World w("p5");
        PerSessionProvider p;
        check(w.bind(p, LiveShellLimits{5min, 30min, 100}).has_value(), "P5 setup: bind");
        (void)shell(p, "noop");
        w.clock.advance(6min);
        EffectContext ctx = ctx_for("owner");
        (void)shell_tool(p, ctx);  // on_context
        check(w.counters->closes == 1 && !p.holds_environment(), "P5: max_idle releases at on_context");
        (void)shell(p, "noop");
        for (int i = 0; i < 8; ++i) {
            w.clock.advance(4min);
            (void)shell(p, "noop");
        }
        check(w.counters->opens >= 3 && w.counters->closes >= 2,
              "P5: max_alive releases a busy shell at shell_exec and the command runs in a fresh one");
    }

    // ---- P6
    {
        World w("p6");
        auto small_open = *agentengine::rt::AsyncQuota<LiveShellOpen>::mint_root(w.authority, w.owner, 3);
        PerRunProvider p;
        check(w.bind(p, LiveShellLimits{1h, 10h, 100}, &small_open).has_value(), "P6 setup: bind");
        (void)shell(p, "noop");                                // open 1
        auto r1 = shell(p, "noop", "owner", true);             // open 2
        auto r2 = shell(p, "noop", "owner", true);             // open 3
        check(r1.has_value() && r1->reopened && r2.has_value() && r2->reopened && small_open.remaining() == 0,
              "P6: restart re-opens and is charged to LiveShellOpen");
        std::uint64_t const run_before = w.run_q.remaining();
        int const execs_before = w.counters->execs;
        auto refused = shell(p, "write never.txt x", "owner", true);
        check(!refused.has_value() && refused.error().code == "async_quota.exhausted" &&
                  w.counters->execs == execs_before && w.run_q.remaining() == run_before,
              "P6: once spent, a restart is refused, nothing runs, RunCost is refunded");
    }

    // ---- P7
    {
        World w("p7");
        PerSessionProvider p;
        check(w.bind(p).has_value(), "P7 setup: bind");
        (void)shell(p, "write keep.txt k");
        auto at_a = shell(p, "cd /workspace/a");  // turn n: cwd /workspace/a
        (void)shell(p, "write later.txt l");
        (void)shell(p, "cd /workspace/b");
        auto reset = drive(p.reset_to_turn(at_a->turn_index, w.owner, w.reset_q));
        check(reset.has_value(), "P7 setup: reset_to_turn");
        auto after = shell(p, "noop");
        check(after.has_value() && after->reopened && after->output.find("in /workspace/a") != std::string::npos &&
                  !tree_has(w.ledger, after->tree_digest, w.owner, "later.txt") &&
                  tree_has(w.ledger, after->tree_digest, w.owner, "keep.txt"),
              "P7: reset_to_turn(n) re-opens with turn n's files and cwd");
        // Equal tree: two cwd-only commands, then reset to the first.
        auto x = shell(p, "cd /workspace/x");
        (void)shell(p, "cd /workspace/y");
        auto reset2 = drive(p.reset_to_turn(x->turn_index, w.owner, w.reset_q));
        auto again = shell(p, "noop");
        check(reset2.has_value() && again.has_value() && again->reopened && again->output.find("in /workspace/x") != std::string::npos,
              "P7: ... also when turn n's tree equals the head's (sync on the checkpoint)");
    }

    // ---- P8
    {
        World w("p8");
        PerSessionProvider p;
        check(w.bind(p).has_value(), "P8 setup: bind");
        (void)shell(p, "cd /workspace/kept");
        auto lost = shell(p, "lose");
        check(lost.has_value() && lost->shell_lost && lost->ok, "P8 setup: the shell is lost, the workspace committed");
        auto next = shell(p, "noop");
        check(next.has_value() && next->reopened && next->output.find("in /workspace/kept") != std::string::npos,
              "P8: the re-open replays the last COMPLETED command's cwd");
    }

    // ---- P9
    {
        World w("p9");
        auto tiny = *agentengine::rt::AsyncQuota<StorageBytes>::mint_root(w.authority, w.owner, 1);
        PerSessionProvider p;
        check(w.bind(p, LiveShellLimits{1h, 10h, 100}, nullptr, &tiny).has_value(), "P9 setup: bind");
        auto r = shell(p, "write big.txt payload");
        check(r.has_value() && !r->ok && r->commit_error.rfind("async_quota.exhausted:", 0) == 0 &&
                  r->output.find("write big.txt") != std::string::npos && r->exit_code == 0,
              "P9: a failed commit is a reply with ok=false, the code, and the command's output");
        auto next = shell(p, "noop");
        check(next.has_value() && next->reopened && next->unrecorded_changes_discarded,
              "P9: the next command re-opens and reports the discard");
    }

    // ---- P10
    {
        World w("p10");
        PerRunProvider p;
        check(w.bind(p).has_value(), "P10 setup: bind");
        auto r = shell(p, "write intruder.txt x", "someone-else");
        check(!r.has_value() && r.error().code == "async_quota.unauthorized_spender" && w.counters->opens == 0 &&
                  w.counters->execs == 0,
              "P10: a non-owner principal is refused before anything opens or runs");
        // P10b: a delegate acting on_behalf_of the owner is a different identity -- and would pass the Ledger's
        // ancestry-aware ACL -- so the owner-only RunCost is what refuses it (C6's control: drop that gate and the
        // delegate's command runs).
        // The owner's shell is OPEN first, so no open (and no owner-only LiveShellOpen charge) stands in the way.
        auto own = shell(p, "noop");
        int const execs_before = w.counters->execs;
        auto d = shell(p, "write delegated.txt x", "delegate", false, "owner");
        check(own.has_value() && !d.has_value() && d.error().code == "async_quota.unauthorized_spender" &&
                  w.counters->execs == execs_before,
              "P10b: a delegate on_behalf_of the owner is refused before anything runs in the owner's open shell (C6)");
    }

    // ---- P11
    {
        World w("p11");
        PerSessionProvider p;
        check(w.bind(p).has_value(), "P11 setup: bind");
        (void)shell(p, "write parent.txt p");
        int const opens_before = w.counters->opens;
        PerSessionProvider child = p;
        check(child.is_bound() && child.branch_name() != nullptr && p.branch_name() != nullptr &&
                  *child.branch_name() != *p.branch_name() && !child.holds_environment(),
              "P11: a copy gets a child branch and no live environment");
        auto c = shell(child, "write child.txt c");
        check(c.has_value() && c->reopened && w.counters->opens == opens_before + 1 &&
                  tree_has(w.ledger, c->tree_digest, w.owner, "parent.txt"),
              "P11: the copy opens its own environment, seeded from the parent's head");
        auto pp = shell(p, "noop");
        check(pp.has_value() && !pp->reopened && !tree_has(w.ledger, pp->tree_digest, w.owner, "child.txt"),
              "P11: the original's environment and branch are untouched");
    }

    // ---- P12
    {
        World w("p12");
        PerRunProvider p;
        check(w.bind(p).has_value(), "P12 setup: bind");
        (void)shell(p, "noop");
        w.counters->close_failures_left = 1;
        Recorded rec;
        run_end(p, &rec);
        bool const reported = std::any_of(rec.events.begin(), rec.events.end(), [](auto const& e) {
            return e.stage == "release" && !e.ok && e.error_code == "test.close_failed";
        });
        check(reported && p.release_failures() == 1 && p.holds_environment(),
              "P12: a failed release is reported as a run event and the handle is kept");
        EffectContext ctx = ctx_for("owner");
        (void)shell_tool(p, ctx);
        check(w.counters->closes == 1 && !p.holds_environment(), "P12: the next check point retries and releases");
    }

    // ---- P13
    {
        World w("p13");
        {
            PerSessionProvider p;
            check(w.bind(p).has_value(), "P13 setup: bind");
            (void)shell(p, "noop");
        }
        check(w.counters->closes == 1, "P13: the destructor releases a held environment");
    }

    // ---- P14
    {
        World w("p14");
        PerRunProvider live;
        check(w.bind(live).has_value(), "P14 setup: bind");
        ComposedContextProvider<PerRunProvider, PlainProvider> composed{std::tuple{std::move(live), PlainProvider{}}};
        auto r = shell(composed, "noop");
        check(r.has_value() && r->ok && w.counters->opens == 1, "P14 setup: shell_exec through the composition");
        run_end(composed);
        check(w.counters->closes == 1, "P14: run end reaches a COMPOSED live shell and releases it");
    }

    // ---- P15, P16 (C4): nothing a returned shell_exec reported is lost on any run exit.
    {
        CapabilitySet const held = CapabilitySet::grant_root({Capability{cap::RunCommand{}}});
        auto head_has = [](World& w, std::string const& branch, std::string const& name) {
            auto head = w.ledger.head_checkpoint(branch, w.owner);
            return head.has_value() && tree_has(w.ledger, head->tree, w.owner, name);
        };
        // P15: shell_exec, then the run is CANCELED between turns.
        {
            World w("p15");
            PerRunProvider live;
            check(w.bind(live).has_value(), "P15 setup: bind");
            std::string const branch = *live.branch_name();
            LiveSession s;
            s.initialize("p15", Principal{"owner", ""});
            s.set_capabilities(&held);
            check(s.history_provider().engage(std::tuple<PerRunProvider, CancelProvider>{std::move(live), CancelProvider{}}).has_value(),
                  "P15 setup: engage");
            s.emplace_chat_client().set_script(
                {Step{tool_calls({{"shell_exec", R"({"command":"write kept.txt k"})"}, {"cancel_tool", R"({"unused":0})"}}), {}},
                 Step{tool_calls({{"shell_exec", R"({"command":"write never.txt n"})"}}), {}}});
            g_cancel_target = &s;
            auto r = drive(s.start_run(rt::StartRun{user_text("go")}));
            g_cancel_target = nullptr;
            check(!r.has_value() && head_has(w, branch, "kept.txt") && !head_has(w, branch, "never.txt"),
                  "P15: a run canceled between turns keeps every shell_exec that returned (C4)");
            check(w.counters->closes == 1, "P15: ... and its run end released the shell");
        }
        // P16: shell_exec, then the next model call FAILS the run.
        {
            World w("p16");
            PerRunProvider live;
            check(w.bind(live).has_value(), "P16 setup: bind");
            std::string const branch = *live.branch_name();
            LiveSession s;
            s.initialize("p16", Principal{"owner", ""});
            s.set_capabilities(&held);
            check(s.history_provider().engage(std::tuple<PerRunProvider, CancelProvider>{std::move(live), CancelProvider{}}).has_value(),
                  "P16 setup: engage");
            s.emplace_chat_client().set_script(
                {Step{tool_calls({{"shell_exec", R"({"command":"write kept.txt k"})"}}), {}},
                 Step{std::nullopt, error{failure_class::transient, "backend down", "test.backend_down"}}});
            auto r = drive(s.start_run(rt::StartRun{user_text("go")}));
            check(!r.has_value() && head_has(w, branch, "kept.txt"), "P16: a run that FAILS after a shell_exec keeps its write (C4)");
        }
    }

    // ---- P17 (ADR-209 §15.5 M1): a command the surface refuses AFTER this call opened the environment.
    {
        World w("p17");
        PerRunProvider p;
        check(w.bind(p).has_value(), "P17 setup: bind");
        Recorded rec;
        EffectContext ctx = ctx_for("owner", &rec);
        auto tool = shell_tool(p, ctx);
        auto r = tool ? tool->invoke(json::Value::make_object({{"command", json::Value::make_string("refuse")}}), ctx)
                      : result<json::Value>(std::unexpected(error{failure_class::contract, "no tool", "test.no_tool"}));
        bool const create_audited = std::any_of(rec.events.begin(), rec.events.end(),
                                                [](auto const& e) { return e.stage == "create" && e.ok; });
        check(!r.has_value() && r.error().code == "test.refused" && w.counters->opens == 1 && p.holds_environment() &&
                  create_audited,
              "P17: the environment opened for a refused command is held, counted and audited");
        run_end(p);
        check(w.counters->closes == 1 && !p.holds_environment(), "P17: ... so PerRun releases it at run end");
    }

    // ---- P18 (§15.5): a lost container whose removal FAILED stays held and is retried.
    {
        World w("p18");
        PerSessionProvider p;
        check(w.bind(p).has_value(), "P18 setup: bind");
        (void)shell(p, "noop");
        w.counters->close_failures_left = 1;
        auto r = shell(p, "lose_container");
        check(r.has_value() && r->container_lost && p.holds_environment() && p.release_failures() == 1,
              "P18: a lost container whose removal failed is still held (not silently forgotten)");
        EffectContext ctx = ctx_for("owner");
        (void)shell_tool(p, ctx);
        check(w.counters->closes == 1 && !p.holds_environment(), "P18: the next check point retries the removal");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
