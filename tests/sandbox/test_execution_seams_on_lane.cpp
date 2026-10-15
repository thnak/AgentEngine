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
//
// Every check also asserts the conformer really suspended (`parks` > 0), so a fake that silently completed
// inline could not make these pass.
//
// Positive controls (planted by hand, recorded in ADR-237 §14): driving the fetch with `rt::block_on` inside
// `invoke_via` fails E1 on the lane.

#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/tools/read_content.hpp"
#include "agentengine/trust/capability.hpp"

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

}  // namespace

int main() {
    agentengine::rt::Runtime runtime{agentengine::rt::RuntimeConfig{.lanes = 1}};
    g_runtime = &runtime;
    Parker parker{&runtime.reactor()};

    egress_section(parker);

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
