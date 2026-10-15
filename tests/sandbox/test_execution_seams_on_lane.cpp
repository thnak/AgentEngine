// decisions/ADR-237 §5.1, Phase D (#173): the execution and sandbox seams are `task`-shaped -- `NetEgressBackend`,
// `Runner`, `SandboxBackend`, `ExecutionSurface`, `PersistentShellSurface`.
//
// This file proves the property that conversion exists for: a conformer whose calls GENUINELY SUSPEND (each one
// parks on a real reactor timer before answering, as a proxy client, a remote sandbox or a container daemon
// would) works through the engine's callers, with every call driven ON A RUNTIME LANE (`Runtime::run`). A lane
// refuses `rt::block_on` (`rt.block_on_on_lane`, ADR-237 §4.3), so only a caller that AWAITS the seam passes.
//
//   E1  ReadContent::invoke_via() fetches through a suspending NetEgressBackend.
//   E2  the same tool's own error mapping (a non-2xx status) is unchanged through a suspending backend.
//   R1  a Runner that suspends mid-run still shares `ExecState` BY REFERENCE (010 §3a): the write it makes
//       after resuming is the caller's object, and a second runner sees it.
//   R2  `run_shell` (SessionShellSandbox, the production Runner caller) runs through the real tool pipeline
//       on a lane: its tool body awaits the runner instead of driving it synchronously.
//   S1  a SandboxBackend that suspends in create/exec/destroy, registered in SandboxBackendRegistry and
//       resolved by name, is driven through the registry's type-erased entry: create -> exec -> exec -> destroy.
//   S2  per-instance state survives those suspensions (the registry's Revision 1 regression: the second exec
//       sees the first's state), and a destroyed handle is gone.
//
// Every check also asserts the conformer really suspended (`parks` > 0), so a fake that silently completed
// inline could not make these pass.
//
// Positive controls (planted by hand, recorded in ADR-237 §14): driving the fetch with `rt::block_on` inside
// `invoke_via` fails E1 on the lane; driving `shell_.run()` with `rt::block_on` in `SessionShellSandbox::run`
// fails R2; a registry `exec` entry that drives the backend with `rt::block_on` fails S1.

#include <chrono>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <filesystem>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "agentengine/sandbox/sandbox_backend_registry.hpp"
#include "agentengine/tools/read_content.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/session_shell_wiring.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

using namespace std::chrono_literals;

// Parks the calling coroutine on the runtime's reactor for a moment -- a real suspension, resumed later from
// the reactor, the way a network round-trip would be.
struct Parker {
    agentengine::pal::Reactor* reactor = nullptr;
    std::size_t parks = 0;
    agentengine::task<void> park() {
        ++parks;
        (void)co_await agentengine::rt::sleep_for(*reactor, 1ms);
    }
};

agentengine::rt::Runtime* g_runtime = nullptr;

// Runs `t` as a root on a runtime lane and waits for it from this (main) thread.
template <class T>
T run(agentengine::task<T> t) {
    return g_runtime->run(std::move(t));
}

// ---- E: NetEgressBackend -----------------------------------------------------------------------------------

struct SuspendingEgress {
    static constexpr agentengine::sandbox::NetEgressTraits traits{"suspending-egress", true};
    Parker* parker = nullptr;
    agentengine::sandbox::NetEgressResponse response{200, {{"Content-Type", "text/plain"}}, "hello"};

    agentengine::task<agentengine::result<agentengine::sandbox::NetEgressResponse>> fetch(
        agentengine::sandbox::NetEgressRequest, agentengine::cap::NetOut) const {
        co_await parker->park();
        co_return response;
    }
};
static_assert(agentengine::sandbox::NetEgressBackend<SuspendingEgress>);

void egress_section(Parker& parker) {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::NetOut{{"example.com:80:http"}, std::nullopt, {}}});
    agentengine::EffectContext ctx{};
    ctx.principal = agentengine::Principal{"p-lane", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    SuspendingEgress backend{&parker};
    ReadContent::Args const args{.url = "http://example.com/data", .path = std::nullopt};
    std::size_t parks_before = parker.parks;
    auto reply = run(ReadContent::invoke_via(args, ctx, backend));
    AE_CHECK(reply.has_value() && reply->preview == "hello",
             "E1: read_content, run on a lane, awaits an egress backend that genuinely suspends");
    AE_CHECK(parker.parks > parks_before, "E1: the egress backend really suspended");

    backend.response.status = 503;
    parks_before = parker.parks;
    auto failed = run(ReadContent::invoke_via(args, ctx, backend));
    AE_CHECK(!failed.has_value() && failed.error().code == "read_content.http_error",
             "E2: a non-2xx status through a suspending backend maps to read_content.http_error, as before");
    AE_CHECK(parker.parks > parks_before, "E2: the egress backend really suspended");
}

// ---- R: Runner ---------------------------------------------------------------------------------------------

// Stands in for a runner behind a worker process: parks, then mutates the shared ExecState the way `cd` and
// `export` would.
struct SuspendingRunner {
    Parker* parker = nullptr;
    agentengine::task<agentengine::result<agentengine::ExecOutcome>> run(agentengine::ExecRequest request,
                                                                         agentengine::ExecState& state,
                                                                         agentengine::EffectContext&) {
        co_await parker->park();
        state.cwd = "/work/" + request.source;
        state.env["SEEN_BY"] = "suspending";
        agentengine::ExecOutcome out;
        out.stdout_text = state.cwd;
        co_return out;
    }
};
static_assert(agentengine::Runner<SuspendingRunner>);

struct ReadingRunner {
    agentengine::task<agentengine::result<agentengine::ExecOutcome>> run(agentengine::ExecRequest,
                                                                         agentengine::ExecState& state,
                                                                         agentengine::EffectContext&) {
        agentengine::ExecOutcome out;
        out.stdout_text = state.cwd + "|" + (state.env.contains("SEEN_BY") ? state.env.at("SEEN_BY") : "");
        co_return out;
    }
};
static_assert(agentengine::Runner<ReadingRunner>);

agentengine::task<agentengine::result<agentengine::ExecOutcome>> run_both(SuspendingRunner& a, ReadingRunner& b,
                                                                          agentengine::ExecState& state,
                                                                          agentengine::EffectContext& ctx) {
    auto first = co_await a.run(agentengine::ExecRequest{"shell", "sub"}, state, ctx);
    if (!first) co_return first;
    co_return co_await b.run(agentengine::ExecRequest{"python", ""}, state, ctx);
}

void runner_section(Parker& parker) {
    agentengine::ExecState state;
    agentengine::EffectContext ctx{};
    SuspendingRunner suspending{&parker};
    ReadingRunner reading;
    std::size_t const parks_before = parker.parks;
    auto out = run(run_both(suspending, reading, state, ctx));
    AE_CHECK(out.has_value() && out->stdout_text == "/work/sub|suspending",
             "R1: a write made after a suspension lands in the caller's ExecState, seen by the next runner");
    AE_CHECK(state.cwd == "/work/sub", "R1: the caller's own ExecState object holds it (by reference, not a copy)");
    AE_CHECK(parker.parks > parks_before, "R1: the runner really suspended");

    namespace fs = std::filesystem;
    fs::path const scratch = fs::temp_directory_path() / "ae_execution_seams_on_lane_shell";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    auto sandbox = agentengine::SessionShellSandbox::create(scratch);
    AE_CHECK(sandbox.has_value(), "R2 setup: SessionShellSandbox::create succeeds");
    if (!sandbox.has_value()) return;
    auto const table = agentengine::ToolTable::from_descriptors({(*sandbox)->tool_descriptor()});
    agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root({
        agentengine::Capability{agentengine::cap::FsRead{"work", "", std::nullopt}},
        agentengine::Capability{agentengine::cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
    });
    agentengine::EffectContext shell_ctx{};
    shell_ctx.principal = agentengine::Principal{"p-lane", ""};
    shell_ctx.capabilities = agentengine::borrow_capabilities(held);
    agentengine::ToolCallRequest const req{"call-lane", "run_shell",
                                           *agentengine::json::parse(R"({"source":"echo on-lane > f.txt"})"), false};
    agentengine::ToolInvocationAudit audit;
    agentengine::ToolResult const result = run(agentengine::invoke_tool(table, held, req, shell_ctx, nullptr, &audit));
    AE_CHECK(!result.is_error, "R2: run_shell, invoked through the tool pipeline on a lane, succeeds");
    AE_CHECK(fs::exists(scratch / "f.txt"), "R2: and its command really ran in the session's sandbox");
    fs::remove_all(scratch, ec);
}

// ---- S: SandboxBackend --------------------------------------------------------------------------------------

// Stands in for a remote sandbox (a microVM host, a container daemon): every call parks first.
struct SuspendingBackend {
    static constexpr agentengine::ProfileTraits traits{1, agentengine::platform_id::windows_x86_64 |
                                                              agentengine::platform_id::linux_x86_64,
                                                       agentengine::cold_start_class::milliseconds};
    Parker* parker = nullptr;
    std::map<std::string, int> execs;  // per-instance state: exec count per live handle
    int next = 0;

    agentengine::task<agentengine::result<agentengine::SandboxHandle>> create(agentengine::SandboxSpec,
                                                                              agentengine::EffectContext&) {
        co_await parker->park();
        std::string id = "h" + std::to_string(++next);
        execs[id] = 0;
        co_return agentengine::SandboxHandle{id};
    }
    agentengine::task<agentengine::result<agentengine::ExecOutcome>> exec(agentengine::SandboxHandle& handle,
                                                                          agentengine::ExecRequest request,
                                                                          agentengine::EffectContext&) {
        co_await parker->park();
        auto it = execs.find(handle.opaque_id);
        if (it == execs.end()) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::contract, "unknown handle",
                                                         "test.unknown_handle"});
        }
        agentengine::ExecOutcome out;
        out.stdout_text = request.source + "#" + std::to_string(++it->second);
        co_return out;
    }
    agentengine::task<void> destroy(agentengine::SandboxHandle& handle) {
        co_await parker->park();
        execs.erase(handle.opaque_id);
    }
};
static_assert(agentengine::SandboxBackend<SuspendingBackend>);

agentengine::task<agentengine::result<std::string>> drive_registered(agentengine::RegisteredSandboxBackend const& entry,
                                                                     agentengine::EffectContext& ctx) {
    auto handle = co_await entry.create(agentengine::SandboxSpec{}, ctx);
    if (!handle) co_return std::unexpected(handle.error());
    auto first = co_await entry.exec(*handle, agentengine::ExecRequest{"native", "a"}, ctx);
    if (!first) co_return std::unexpected(first.error());
    auto second = co_await entry.exec(*handle, agentengine::ExecRequest{"native", "b"}, ctx);
    if (!second) co_return std::unexpected(second.error());
    co_await entry.destroy(*handle);
    auto after = co_await entry.exec(*handle, agentengine::ExecRequest{"native", "c"}, ctx);
    co_return first->stdout_text + "," + second->stdout_text + "," + (after ? "still-live" : after.error().code);
}

void backend_section(Parker& parker) {
    auto instance = std::make_shared<SuspendingBackend>();
    instance->parker = &parker;
    agentengine::SandboxBackendRegistry registry;
    AE_CHECK(registry.register_backend("suspending", instance).has_value(), "S1 setup: the backend registers");
    auto entry = registry.resolve_named(agentengine::HostSandboxSelection{"suspending"});
    AE_CHECK(entry.has_value(), "S1 setup: resolved by name");
    if (!entry.has_value()) return;
    agentengine::EffectContext ctx{};
    std::size_t const parks_before = parker.parks;
    auto out = run(drive_registered(**entry, ctx));
    AE_CHECK(out.has_value(), "S1: create -> exec -> exec -> destroy through the registry entry, on a lane");
    AE_CHECK(parker.parks >= parks_before + 5, "S1: every one of the five calls really suspended");
    AE_CHECK(out.has_value() && *out == "a#1,b#2,test.unknown_handle",
             "S2: the second exec saw the first's state, and the destroyed handle is gone");
    AE_CHECK(instance->execs.empty(), "S2: destroy reached the one registered instance");
}

}  // namespace

int main() {
    agentengine::rt::Runtime runtime{agentengine::rt::RuntimeConfig{.lanes = 1}};
    g_runtime = &runtime;
    Parker parker{&runtime.reactor()};

    egress_section(parker);
    runner_section(parker);
    backend_section(parker);

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
