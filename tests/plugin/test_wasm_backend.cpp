// Milestone 2 Phase D tasks D3 and D5 (docs/planning/milestone-2-tools-capabilities-sandbox-
// breakdown.md), decisions/archive/ADR-010-wasm-component-host-manifest-capability-binding.md §7.3 --
// proves the real WasmBackend implementation (src/backends/wasm/wasm_backend.{hpp,cpp}) against a
// genuinely compiled `ae:tool` component (tests/fixtures/wasm_ae_tool_fixture/), not a hand-crafted
// stub. D5 extended the fixture with four tools ("read-file"/"write-file"/"fetch"/"get-secret",
// importing ae:tool/{fs,http,secrets}) purely so its own gated-callback probes below have something
// real to call -- the component's whole import set (capability/fs/http/secrets/clock/types) now
// applies to every test in this file, not just the ones that exercise the new tools.
//
//   1. Positive: a manifest whose requested capabilities cover the component's real imports loads,
//      lists its real tools, and invoke_tool() returns a real computed result for both a
//      zero-capability tool ("echo") and a capability-gated one ("now", ae:tool/clock).
//   2. Negative, branch 1 (009 §10 G2 / ADR-010 claim 2, "manifest under-requests"): a manifest that
//      omits Clock, while the operator would have granted it, fails closed at load_component() with
//      `wasm.manifest_capability_not_requested`, never reaching instantiate.
//   3. Negative, branch 2 (D5 -- the same claim's other half, untested by D3): a manifest that DOES
//      request FsRead, but the operator's CapabilitySet does not grant it, fails closed with
//      `wasm.operator_grant_missing`.
//   4. Capability-kind confusion (ADR-010 §5 F3 / claim 4), clock: a manifest that grants Entropy
//      before Clock, so the guest's first capability-handle slot is bound to the WRONG kind for what
//      "now" calls -- proven via the real fixture and real host callback, not a test-only backdoor.
//   5. Capability-kind confusion, the other four gated callbacks (D5 closes claim 4's remaining gap
//      -- D3's ADR explicitly left this proven for only one of five callbacks): for each of
//      fs-read/fs-write/http-request/resolve-secret, one probe places the matching capability first
//      (right kind -- reaches the callback's real behavior, proving the kind check passed) and one
//      places a mismatched capability first (wrong kind -- rejected before reaching it), against the
//      real fixture's read-file/write-file/fetch/get-secret tools. fs-read/fs-write/resolve-secret are
//      still stubs (reaching them traps with "not implemented in M2's minimal host"); http-request is
//      no longer one (decisions/archive/ADR-011-first-party-egress-proxy.md, M2 Phase F task F1) -- its
//      right-kind probe is a dedicated block below, not `probe_gated_callback`, since a real result
//      no longer traps.
//   6. wall_ms is a real, measured kill (claim 5): the "spin" tool loops forever; a low wall_ms
//      limit must actually interrupt it within a bounded, measured time.
//
// SKIPs (CTest SKIP_RETURN_CODE 77, tests/CMakeLists.txt) if the fixture wasn't built -- the
// cargo-component toolchain is not assumed present on every machine running this suite.

#include "agentengine/rt/block_on.hpp"
#include "agentengine/pal/net.hpp"
#include "agentengine/sandbox/detail/net_egress_impl.hpp"
#include "agentengine/sandbox/testing/net_egress_proxy_testing.hpp"
#include "backends/wasm/wasm_backend.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#include <type_traits>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

using namespace agentengine;
using namespace agentengine::wasm;

// The host egress seam takes only a NetEgressBackend conformer (which runs the allowlist gate itself): an arbitrary callable
// -- one that could skip the gate -- cannot become an EgressBlockingFetch. Mutant: make its constructor public -> fails.
namespace seam_shape {
using Erased = std::function<agentengine::result<agentengine::sandbox::NetEgressResponse>(
    agentengine::sandbox::NetEgressRequest, agentengine::cap::NetOut, agentengine::sandbox::EgressCallControl)>;
static_assert(!std::is_constructible_v<agentengine::wasm::EgressBlockingFetch, Erased>,
              "a raw callable must not be accepted as the WASM egress backend");
static_assert(!std::is_convertible_v<Erased, agentengine::wasm::EgressBlockingFetch>,
              "a raw callable must not convert to the WASM egress backend");
}  // namespace seam_shape

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

std::vector<std::uint8_t> read_fixture() {
    std::ifstream f(AE_WASM_FIXTURE_PATH, std::ios::binary);
    if (!f) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

EffectContext make_ctx() {
    EffectContext ctx;
    ctx.trace_id = "test-trace";
    ctx.span_id = "test-span";
    return ctx;
}

// The D5-extended fixture's real, whole-component import set (capability/types are always_ok and
// need no capability; fs/http/secrets/clock do) -- every test in this file that loads the component
// at all must cover this exact set, regardless of which tool it goes on to invoke (ADR-010 §3.2's
// found imprecision: imports are component-wide, not per-tool). Clock listed first so tests that
// invoke "now" (which reads request.capabilities.first()) keep getting the kind it expects; the
// per-callback probes below (§5) define their own orderings deliberately instead of using this one.
std::vector<Capability> const kAllRequiredCapabilities = {
    cap::Clock{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{},
};

// One gated-callback kind-check probe (D5, closing ADR-010 claim 4's remaining four-callback gap):
// loads the real fixture with `order` as both the operator grant and the manifest request -- so
// `order[0]` is exactly what `tool`'s `request.capabilities.first()` resolves to inside the guest
// (invoke_tool() binds capabilities in manifest.requested_capabilities order) -- invokes `tool`, and
// checks the resulting failure's message contains `expect_substring`. Every probe's `order` still
// contains the component's whole real import set (kAllRequiredCapabilities, reordered), or
// load_component() itself would reject it before the probe ever reaches invoke_tool().
void probe_gated_callback(std::vector<std::uint8_t> const& bytes, std::string const& label,
                           std::string const& tool, std::vector<Capability> const& order,
                           std::string const& expect_substring) {
    WasmBackend backend;
    SandboxSpec spec;
    spec.capabilities = CapabilitySet::grant_root(order);
    EffectContext ctx = make_ctx();
    auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
    if (!handle) {
        AE_CHECK(false, label + ": create() succeeds");
        return;
    }

    PluginManifest manifest;
    manifest.id = "test.gated." + tool + "." + label;
    manifest.version = "0.1.0";
    manifest.world = plugin_world::tool;
    manifest.requested_capabilities = order;

    auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
    if (!loaded) std::cerr << "  (" << label << " load error: " << loaded.error().code << ": " << loaded.error().message << ")\n";
    AE_CHECK(loaded.has_value(), label + ": load_component() succeeds");
    if (!loaded) {
        agentengine::rt::block_on(backend.destroy(*handle));
        return;
    }

    auto result = backend.invoke_tool(*handle, ToolInvokeRequest{tool, ""}, ctx);
    AE_CHECK(!result.has_value(), label + ": " + tool + " call fails");
    if (result) {
        std::cerr << "  (" << label << ": expected a failure, call unexpectedly succeeded)\n";
    } else {
        std::cerr << "  (" << label << " message: " << result.error().message << ")\n";
        bool const matches = result.error().message.find(expect_substring) != std::string::npos;
        AE_CHECK(matches, label + ": failure reason contains \"" + expect_substring + "\"");
    }

    agentengine::rt::block_on(backend.destroy(*handle));
}


// ADR-241 S2 red-team r5 M1/m3: a loopback listener that ACCEPTS and counts, so "the production policy refused it" is
// provable as `accept_count() == 0` (a never-accepting listener would let a connect "succeed" from the backlog).
class AcceptCounter {
public:
    AcceptCounter() {
        auto l = agentengine::pal::tcp_listen(static_cast<std::uint64_t>((127u << 24) | 1u), 0);
        if (!l) return;
        fd_ = *l;
        port_ = *agentengine::pal::local_port(fd_);
        ok_ = true;
        thread_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested()) {
                auto a = agentengine::pal::accept_one(fd_);
                if (!a) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                count_.fetch_add(1);
                agentengine::pal::close_fd(*a);
            }
        });
    }
    ~AcceptCounter() {
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
        if (ok_) agentengine::pal::close_fd(fd_);
    }
    AcceptCounter(AcceptCounter const&) = delete;
    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int accept_count() const { return count_.load(); }

private:
    bool ok_ = false;
    agentengine::pal::fd_t fd_{};
    std::uint16_t port_ = 0;
    std::atomic<int> count_{0};
    std::jthread thread_;
};

// A hung egress call (a latch that is never counted down) must FAIL the test within a bound, not hang the suite:
// if the guard is not disarmed in time, the process exits non-zero.
class HangGuard {
public:
    explicit HangGuard(std::chrono::seconds limit) {
        thread_ = std::jthread([limit](std::stop_token st) {
            std::mutex m;
            std::condition_variable_any cv;
            std::unique_lock<std::mutex> lock(m);
            if (!cv.wait_for(lock, st, limit, [] { return false; }) && !st.stop_requested()) {
                std::cerr << "FAIL: the WASM egress row hung past " << limit.count() << " s (latch never counted down?)\n";
                std::_Exit(3);
            }
        });
    }
    ~HangGuard() { thread_.request_stop(); }

private:
    std::jthread thread_;
};

// One `fetch` call from the real fixture guest, through the production HostEgressProxy, against a single-entry loopback
// grant. Returns the guest's text (it names the WIT http-error case) or "" on a call-level failure.
std::string fetch_via_loopback_grant(std::vector<std::uint8_t> const& bytes, std::uint16_t port, std::uint64_t wall_ms,
                                     std::string const& label, std::chrono::milliseconds* elapsed) {
    WasmBackend backend;
    SandboxSpec spec;
    cap::NetOut net;
    net.host_allowlist = {"127.0.0.1:" + std::to_string(port) + ":http"};
    std::vector<Capability> const order = {net, cap::FsRead{}, cap::FsWrite{}, cap::Secret{}, cap::Clock{}};
    spec.capabilities = CapabilitySet::grant_root(order);
    spec.limits.wall_ms = wall_ms;
    EffectContext ctx = make_ctx();
    auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
    AE_CHECK(handle.has_value(), label + ": create() succeeds");
    if (!handle) return "";
    PluginManifest manifest;
    manifest.id = "test.egress." + label;
    manifest.version = "0.1.0";
    manifest.world = plugin_world::tool;
    manifest.requested_capabilities = order;
    auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
    AE_CHECK(loaded.has_value(), label + ": load_component() succeeds");
    std::string text;
    if (loaded) {
        auto const started = std::chrono::steady_clock::now();
        auto result = backend.invoke_tool(*handle, ToolInvokeRequest{"fetch", ""}, ctx);
        if (elapsed != nullptr) {
            *elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        }
        if (!result) std::cerr << "  (" << label << " error: " << result.error().code << ": " << result.error().message << ")\n";
        AE_CHECK(result.has_value() && !result->is_error && result->content.size() == 1, label + ": the call returns a structured result");
        if (result && !result->content.empty()) {
            if (auto const* t = std::get_if<Text>(&result->content[0].value)) text = t->text;
        }
    }
    agentengine::rt::block_on(backend.destroy(*handle));
    return text;
}

// ADR-241 follow-up (G7/G17 on the WASM edge): a loopback peer that ACCEPTS and then never answers (and never closes until
// the test ends), so a parked `http-request` can only be released by the call's deadline or its cancellation.
class SilentPeer {
public:
    SilentPeer() {
        auto l = agentengine::pal::tcp_listen(static_cast<std::uint64_t>((127u << 24) | 1u), 0);
        if (!l) return;
        fd_ = *l;
        port_ = *agentengine::pal::local_port(fd_);
        ok_ = true;
        thread_ = std::jthread([this](std::stop_token st) {
            std::vector<agentengine::pal::fd_t> held;
            while (!st.stop_requested()) {
                auto a = agentengine::pal::accept_one(fd_);
                if (!a) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                count_.fetch_add(1);
                held.push_back(*a);
            }
            for (auto fd : held) agentengine::pal::close_fd(fd);
        });
    }
    ~SilentPeer() {
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
        if (ok_) agentengine::pal::close_fd(fd_);
    }
    SilentPeer(SilentPeer const&) = delete;
    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int accept_count() const { return count_.load(); }

private:
    bool ok_ = false;
    agentengine::pal::fd_t fd_{};
    std::uint16_t port_ = 0;
    std::atomic<int> count_{0};
    std::jthread thread_;
};

// A `TestEgressProxy` that permits loopback, injected through the WasmBackend seam. Shared so the row can read gate_calls().
std::shared_ptr<sandbox::TestEgressProxy> make_loopback_proxy() {
    auto p = std::make_shared<sandbox::TestEgressProxy>();
    p->address_policy = [](agentengine::pal::IpAddress const&) noexcept { return true; };
    return p;
}

// One `fetch-with` call ("METHOD SIZE") through a WasmBackend whose egress is `proxy`. `stop_after`, when non-zero, requests
// cancellation of the call's EffectContext from another thread after that long. Returns the guest's text.
std::string fetch_with_through(std::vector<std::uint8_t> const& bytes, std::shared_ptr<sandbox::TestEgressProxy> proxy,
                               std::uint16_t port, std::uint64_t wall_ms, std::string const& args,
                               std::chrono::milliseconds stop_after, std::string const& label, std::chrono::milliseconds* elapsed,
                               std::string* call_error = nullptr) {
    WasmBackend backend(erase_egress_backend(std::shared_ptr<sandbox::TestEgressProxy const>(proxy)));
    SandboxSpec spec;
    cap::NetOut net;
    net.host_allowlist = {"127.0.0.1:" + std::to_string(port) + ":http"};
    std::vector<Capability> const order = {net, cap::FsRead{}, cap::FsWrite{}, cap::Secret{}, cap::Clock{}};
    spec.capabilities = CapabilitySet::grant_root(order);
    spec.limits.wall_ms = wall_ms;
    EffectContext ctx = make_ctx();
    std::stop_source stopper;
    ctx.cancellation = stopper.get_token();
    auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
    AE_CHECK(handle.has_value(), label + ": create() succeeds");
    if (!handle) return "";
    PluginManifest manifest;
    manifest.id = "test.egress.seam." + label;
    manifest.version = "0.1.0";
    manifest.world = plugin_world::tool;
    manifest.requested_capabilities = order;
    auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
    AE_CHECK(loaded.has_value(), label + ": load_component() succeeds");
    std::string text;
    if (loaded) {
        std::jthread canceller;
        if (stop_after.count() > 0) {
            canceller = std::jthread([&stopper, stop_after] {
                std::this_thread::sleep_for(stop_after);
                stopper.request_stop();
            });
        }
        auto const started = std::chrono::steady_clock::now();
        auto result = backend.invoke_tool(*handle, ToolInvokeRequest{"fetch-with", args}, ctx);
        if (elapsed != nullptr) {
            *elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        }
        if (!result && call_error != nullptr) {
            *call_error = result.error().code + ": " + result.error().message;  // the caller asserts on it
        } else {
            if (!result) std::cerr << "  (" << label << " error: " << result.error().code << ": " << result.error().message << ")\n";
            AE_CHECK(result.has_value() && !result->is_error && result->content.size() == 1, label + ": the call returns a structured result");
        }
        if (result && !result->content.empty()) {
            if (auto const* t = std::get_if<Text>(&result->content[0].value)) text = t->text;
        }
    }
    agentengine::rt::block_on(backend.destroy(*handle));
    return text;
}

void check_admission_idle(std::string const& label) {
    auto const snap = sandbox::egress_detail::egress_admission_snapshot();
    AE_CHECK(snap.global == 0 && snap.runs == 0 && snap.principals == 0 && snap.unattributed_run == 0 &&
                 snap.unattributed_principal == 0,
             label + ": every admission counter is back to 0 afterwards");
}

}  // namespace

int main() {
    std::vector<std::uint8_t> const bytes = read_fixture();
    if (bytes.empty()) {
        std::cerr << "SKIP: " << AE_WASM_FIXTURE_PATH
                  << " not found -- cargo-component toolchain unavailable, see "
                     "tests/fixtures/wasm_ae_tool_fixture/README.md\n";
        return 77;
    }
    std::cout << "fixture: " << bytes.size() << " bytes\n";

    // -- 1. Positive: full grant -----------------------------------------------------------------
    {
        WasmBackend backend;
        SandboxSpec spec;
        // The D5-extended fixture now imports fs/http/secrets too (four new tools calling them), so
        // the whole-component import check needs all five gated kinds granted, not just Clock.
        spec.capabilities = CapabilitySet::grant_root({cap::Clock{}, cap::Entropy{}, cap::FsRead{},
                                                         cap::FsWrite{}, cap::NetOut{}, cap::Secret{}});
        spec.limits.memory_bytes = 64ull * 1024 * 1024;

        EffectContext ctx = make_ctx();
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        AE_CHECK(handle.has_value(), "positive: create() succeeds");
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.echo-now-spin";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        manifest.requested_capabilities = kAllRequiredCapabilities;
        manifest.memory_bytes_limit = spec.limits.memory_bytes;

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(loaded.has_value(), "positive: load_component() succeeds (manifest covers real imports)");

        auto tools = backend.list_tools(*handle, ctx);
        if (!tools) std::cerr << "  (list_tools error: " << tools.error().code << ": " << tools.error().message << ")\n";
        // D5 added four tools (read-file/write-file/fetch/get-secret) alongside D3's original three.
        AE_CHECK(tools.has_value() && tools->size() == 8, "positive: list_tools() returns all 8 real tools");
        if (tools) {
            bool has_echo = false, has_now = false, has_spin = false, echo_parallelizable = false;
            bool has_read = false, has_write = false, has_fetch = false, has_secret = false;
            for (auto const& t : *tools) {
                if (t.name == "echo") { has_echo = true; echo_parallelizable = t.parallelizable; }
                if (t.name == "now") has_now = true;
                if (t.name == "spin") has_spin = true;
                if (t.name == "read-file") has_read = true;
                if (t.name == "write-file") has_write = true;
                if (t.name == "fetch") has_fetch = true;
                if (t.name == "get-secret") has_secret = true;
            }
            AE_CHECK(has_echo && has_now && has_spin && has_read && has_write && has_fetch && has_secret,
                      "positive: tool names match the fixture's real exports");
            AE_CHECK(echo_parallelizable, "positive: echo's parallelizable flag round-trips true");
        }

        auto echo_result = backend.invoke_tool(*handle, ToolInvokeRequest{"echo", "hello from the host"}, ctx);
        if (!echo_result) std::cerr << "  (echo error: " << echo_result.error().code << ": " << echo_result.error().message << ")\n";
        AE_CHECK(echo_result.has_value() && !echo_result->is_error && echo_result->content.size() == 1,
                  "positive: echo invoke succeeds with one content item");
        if (echo_result && !echo_result->content.empty()) {
            auto const* text = std::get_if<Text>(&echo_result->content[0].value);
            AE_CHECK(text != nullptr && text->text == "hello from the host",
                      "positive: echo returns the exact input text (real computation, not a stub)");
        }

        auto const before_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
        auto now_result = backend.invoke_tool(*handle, ToolInvokeRequest{"now", ""}, ctx);
        auto const after_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
        AE_CHECK(now_result.has_value() && !now_result->is_error && now_result->content.size() == 1,
                  "positive: now invoke succeeds (real ae:tool/clock host call)");
        if (now_result && !now_result->content.empty()) {
            auto const* data = std::get_if<Data>(&now_result->content[0].value);
            AE_CHECK(data != nullptr, "positive: now returns a Data content item");
            if (data) {
                long long const millis = std::atoll(data->json.c_str());
                AE_CHECK(millis >= before_now && millis <= after_now,
                          "positive: now returns a plausible real timestamp, not a placeholder");
            }
        }

        agentengine::rt::block_on(backend.destroy(*handle));
    }

    // -- 2. Negative, branch 1: manifest omits Clock, component still imports ae:tool/clock ------
    {
        WasmBackend backend;
        SandboxSpec spec;
        // Operator would allow everything the component needs, Clock included...
        spec.capabilities = CapabilitySet::grant_root(
            {cap::Clock{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}});
        EffectContext ctx = make_ctx();
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        AE_CHECK(handle.has_value(), "negative: create() succeeds");
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.negative";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        // ...but the MANIFEST requests everything else and deliberately omits Clock -- fail closed,
        // and specifically on Clock (the only uncovered import), not some other one.
        manifest.requested_capabilities = {cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}};

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(!loaded.has_value(), "negative: load_component() fails closed");
        if (!loaded) {
            std::cerr << "  (negative error: " << loaded.error().code << ": " << loaded.error().message << ")\n";
            AE_CHECK(loaded.error().code == "wasm.manifest_capability_not_requested",
                      "negative: specific diagnosis naming the offending import, not a generic refusal");
        }

        // No partial success state: list_tools()/invoke_tool() on this never-loaded handle must
        // also refuse, proving the rejection really happened before anything else was set up.
        auto tools = backend.list_tools(*handle, ctx);
        AE_CHECK(!tools.has_value(), "negative: list_tools() on a rejected handle also fails (no partial load)");

        agentengine::rt::block_on(backend.destroy(*handle));
    }

    // -- 3. Negative, branch 2 (D5, ADR-010 claim 2's other half, untested by D3): manifest ------
    //       requests Clock, but the operator's own CapabilitySet does not grant it.
    //
    //       Clock (not FsRead) is the omitted kind deliberately: fs-read and fs-write share the
    //       same `ae:tool/fs` interface class (interface_covered() checks the *interface*, not the
    //       specific function), so granting FsWrite alone would still satisfy the fs import and this
    //       probe would not actually exercise the operator-side rejection -- a real mistake this
    //       task's own first attempt made and caught by re-running the test, not by inspection.
    //       Clock has no sibling capability kind, so omitting it is unambiguous.
    {
        WasmBackend backend;
        SandboxSpec spec;
        // Operator grants everything the component needs EXCEPT Clock.
        spec.capabilities =
            CapabilitySet::grant_root({cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}});
        EffectContext ctx = make_ctx();
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        AE_CHECK(handle.has_value(), "operator-grant-missing: create() succeeds");
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.operator-grant-missing";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        // Manifest requests the component's real, whole import set, Clock included -- the manifest
        // side of the check passes; the operator side must be what rejects this.
        manifest.requested_capabilities = kAllRequiredCapabilities;

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(!loaded.has_value(), "operator-grant-missing: load_component() fails closed");
        if (!loaded) {
            std::cerr << "  (operator-grant-missing error: " << loaded.error().code << ": "
                       << loaded.error().message << ")\n";
            AE_CHECK(loaded.error().code == "wasm.operator_grant_missing",
                      "operator-grant-missing: specific diagnosis, distinct from the manifest-side code");
        }

        agentengine::rt::block_on(backend.destroy(*handle));
    }

    // -- 4. Capability-kind confusion: Entropy bound where Clock is expected ---------------------
    {
        WasmBackend backend;
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {cap::Entropy{}, cap::Clock{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}});
        EffectContext ctx = make_ctx();
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.kind-confusion";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        // Order matters: capabilities[0] (what the fixture's "now" tool actually calls
        // now-unix-millis with) is bound to Entropy here, not Clock.
        manifest.requested_capabilities = {cap::Entropy{},   cap::Clock{},  cap::FsRead{},
                                            cap::FsWrite{},   cap::NetOut{}, cap::Secret{}};

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        AE_CHECK(loaded.has_value(), "kind-confusion: load_component() still succeeds (both kinds are granted)");

        auto now_result = backend.invoke_tool(*handle, ToolInvokeRequest{"now", ""}, ctx);
        AE_CHECK(!now_result.has_value(),
                  "kind-confusion: now-unix-millis rejects an Entropy handle passed where Clock is required");

        agentengine::rt::block_on(backend.destroy(*handle));
    }

    // -- 5. Capability-kind confusion, the other four gated callbacks (D5 closes ADR-010 claim 4's
    //       remaining gap -- D3 proved this mechanism for now-unix-millis only). For each function,
    //       one probe with the matching capability first (right kind) and one with a mismatched
    //       capability first (wrong kind), against the real fixture's four new tools.
    {
        probe_gated_callback(bytes, "fs-read/right-kind", "read-file",
                              {cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}, cap::Clock{}},
                              "not implemented in M2's minimal host");
        probe_gated_callback(bytes, "fs-read/wrong-kind", "read-file",
                              {cap::FsWrite{}, cap::FsRead{}, cap::NetOut{}, cap::Secret{}, cap::Clock{}},
                              "wrong kind");

        probe_gated_callback(bytes, "fs-write/right-kind", "write-file",
                              {cap::FsWrite{}, cap::FsRead{}, cap::NetOut{}, cap::Secret{}, cap::Clock{}},
                              "not implemented in M2's minimal host");
        probe_gated_callback(bytes, "fs-write/wrong-kind", "write-file",
                              {cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}, cap::Clock{}},
                              "wrong kind");

        // http-request/right-kind is no longer a stub-trap probe (decisions/archive/ADR-011-first-party-
        // egress-proxy.md, M2 Phase F task F1): cb_http_request now returns a REAL, structured
        // result<http-response-data, http-error> instead of trapping once the capability-kind check
        // passes, so probe_gated_callback's "the wasmtime call itself fails" assumption (still
        // correct for fs-read/fs-write/resolve-secret, which remain literal stubs) no longer applies
        // here. The fixture's own "fetch" arm already handles both outcomes without panicking
        // (src/lib.rs: `Ok(_) => ... Err(_) => text_result("fetch: http-error returned")`), so this
        // proves the kind check passed AND the real gate ran: an empty/default `cap::NetOut{}` (no
        // allowlist entry at all) is exactly ADR-011 claim C1's "ambiguous grant" case, which
        // `HostEgressProxy::fetch` rejects as a normal `Err`, not a trap.
        {
            WasmBackend backend;
            SandboxSpec spec;
            std::vector<Capability> const order = {cap::NetOut{}, cap::FsRead{}, cap::FsWrite{}, cap::Secret{}, cap::Clock{}};
            spec.capabilities = CapabilitySet::grant_root(order);
            EffectContext ctx = make_ctx();
            auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
            AE_CHECK(handle.has_value(), "http-request/right-kind: create() succeeds");
            if (handle) {
                PluginManifest manifest;
                manifest.id = "test.gated.fetch.http-request-right-kind";
                manifest.version = "0.1.0";
                manifest.world = plugin_world::tool;
                manifest.requested_capabilities = order;
                auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
                AE_CHECK(loaded.has_value(), "http-request/right-kind: load_component() succeeds");
                if (loaded) {
                    auto result = backend.invoke_tool(*handle, ToolInvokeRequest{"fetch", ""}, ctx);
                    if (!result) std::cerr << "  (http-request/right-kind error: " << result.error().code << ": " << result.error().message << ")\n";
                    AE_CHECK(result.has_value() && !result->is_error && result->content.size() == 1,
                              "http-request/right-kind: fetch call succeeds structurally (no trap -- a real Err value, not a stub)");
                    if (result && !result->content.empty()) {
                        auto const* text = std::get_if<Text>(&result->content[0].value);
                        AE_CHECK(text != nullptr && text->text.rfind("fetch: http-error returned", 0) == 0,
                                  "http-request/right-kind: the real gate rejected the ambiguous (allowlist-empty) NetOut grant");
                    }
                }
                agentengine::rt::block_on(backend.destroy(*handle));
            }
        }
        probe_gated_callback(bytes, "http-request/wrong-kind", "fetch",
                              {cap::FsRead{}, cap::NetOut{}, cap::FsWrite{}, cap::Secret{}, cap::Clock{}},
                              "wrong kind");

        probe_gated_callback(bytes, "resolve-secret/right-kind", "get-secret",
                              {cap::Secret{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Clock{}},
                              "not implemented in M2's minimal host");
        probe_gated_callback(bytes, "resolve-secret/wrong-kind", "get-secret",
                              {cap::Clock{}, cap::Secret{}, cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}},
                              "wrong kind");
    }

    // -- 6. wall_ms is a real, measured kill -------------------------------------------------------
    {
        WasmBackend backend;
        SandboxSpec spec;
        // The fixture's import list is component-wide, not per-tool (ADR-010 §7.5's finding) --
        // every gated import (now clock/fs/http/secrets, since D5 added tools calling all four) must
        // still be granted even though "spin" itself calls none of them, or load_component() fails
        // closed before "spin" is ever reachable.
        spec.capabilities = CapabilitySet::grant_root(kAllRequiredCapabilities);
        spec.limits.wall_ms = 200;
        EffectContext ctx = make_ctx();
        auto handle = agentengine::rt::block_on(backend.create(spec, ctx));
        if (!handle) return 1;

        PluginManifest manifest;
        manifest.id = "test.spin";
        manifest.version = "0.1.0";
        manifest.world = plugin_world::tool;
        manifest.requested_capabilities = kAllRequiredCapabilities;
        manifest.wall_ms_limit = 200;

        auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
        if (!loaded) std::cerr << "  (wall_ms load error: " << loaded.error().code << ": " << loaded.error().message << ")\n";
        AE_CHECK(loaded.has_value(), "wall_ms: load_component() succeeds (spin needs no capability)");

        auto const started = std::chrono::steady_clock::now();
        auto spin_result = backend.invoke_tool(*handle, ToolInvokeRequest{"spin", ""}, ctx);
        auto const elapsed = std::chrono::steady_clock::now() - started;

        AE_CHECK(!spin_result.has_value(), "wall_ms: spin is actually interrupted, not left running forever");
        auto const elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        AE_CHECK(elapsed_ms < 5000,
                  "wall_ms: interrupted within a bounded, measured time (well under a runaway 5s ceiling)");
        std::cout << "  (spin interrupted after " << elapsed_ms << "ms, limit was 200ms)\n";

        agentengine::rt::block_on(backend.destroy(*handle));
    }


    // -- 7. ADR-241 S2 red-team r5 M1 / m3: the WASM edge's whole egress path, end to end, through the PRODUCTION proxy ----
    // A single-entry grant of a loopback listener passes the gate, so `cb_http_request` builds the context, admits with
    // `call->egress`'s key, spawns and parks on the latch, resolves, and the production policy refuses loopback -- the guest
    // sees `host-not-allowlisted` (net.address_blocked), never a connect. Nothing here is a test seam on the backend.
    {
        AcceptCounter listener;
        AE_CHECK(listener.ok(), "egress e2e: loopback listener started");
        if (listener.ok()) {
            HangGuard guard(std::chrono::seconds(90));
            AE_CHECK(sandbox::egress_detail::egress_context_constructions() == 0,
                     "egress e2e: no earlier row built the egress I/O context (the gate rejects before it)");

            // m3: UINT64_MAX means "unbounded" to some hosts; it must clamp, never overflow into a past deadline. A past
            // deadline fails `net.deadline_exceeded` (the guest sees `Timeout`) BEFORE the context is ever built.
            std::chrono::milliseconds took{0};
            auto const huge = fetch_via_loopback_grant(bytes, listener.port(), UINT64_MAX, "wall_ms=UINT64_MAX", &took);
            std::cerr << "  (wall_ms=UINT64_MAX guest saw: " << huge << ")\n";
            AE_CHECK(huge.find("\"host-not-allowlisted\"") != std::string::npos,
                     "m3: wall_ms=UINT64_MAX still lets the call proceed to the policy (not an immediate Timeout)");
            AE_CHECK(sandbox::egress_detail::egress_context_constructions() == 1,
                     "m3: the call got past the deadline step and built the egress I/O context");
            AE_CHECK(listener.accept_count() == 0, "m3: the listener was never connected to");
            check_admission_idle("m3");

            // M1: a normal limit, measured.
            auto const normal = fetch_via_loopback_grant(bytes, listener.port(), 5000, "wall_ms=5000", &took);
            AE_CHECK(normal.find("\"host-not-allowlisted\"") != std::string::npos,
                     "M1: the guest sees host-not-allowlisted (net.address_blocked) from the production policy");
            AE_CHECK(listener.accept_count() == 0, "M1: accept_count() == 0 -- the production policy refused loopback, nothing connected");
            AE_CHECK(took < std::chrono::milliseconds(3000),
                     "M1: the call returns well inside its timeout (took " + std::to_string(took.count()) + " ms)");
            AE_CHECK(sandbox::egress_detail::egress_context_constructions() == 1, "M1: one process-wide egress I/O context");
            check_admission_idle("M1");
        }
    }

    // -- 8. ADR-241 follow-up: G17 / G7 on the WASM edge, through the host-supplied egress seam (TestEgressProxy) ----
    {
        SilentPeer peer;
        AE_CHECK(peer.ok(), "wasm seam: silent loopback peer started");
        if (peer.ok()) {
            HangGuard guard(std::chrono::seconds(60));
            std::chrono::milliseconds took{0};

            // Positive control: the seam really routes to the injected backend -- a bodiless GET reaches the peer
            // (gate_calls() == 1, accept_count() == 1) and parks until wall_ms; the production policy would have refused it.
            {
                auto proxy = make_loopback_proxy();
                auto const text = fetch_with_through(bytes, proxy, peer.port(), 700, "GET 0", std::chrono::milliseconds(0),
                                                     "seam-control", &took);
                std::cerr << "  (seam-control guest saw: " << text << ")\n";
                AE_CHECK(proxy->gate_calls() == 1, "seam: the injected backend's gate ran once (the seam is wired)");
                AE_CHECK(peer.accept_count() == 1, "seam: the silent peer was connected to (loopback permitted by the injected proxy)");
            }

            // G7-WASM timing: wall_ms=2000 against a peer that accepts and never answers -> released by the deadline.
            {
                auto proxy = make_loopback_proxy();
                auto const text = fetch_with_through(bytes, proxy, peer.port(), 2000, "GET 0", std::chrono::milliseconds(0),
                                                     "G7-deadline", &took);
                std::cerr << "  (G7 deadline guest saw: " << text << ", took " << took.count() << " ms)\n";
                AE_CHECK(text.find("name: \"timeout\"") != std::string::npos, "G7: the guest sees the WIT timeout http-error");
                AE_CHECK(took < std::chrono::milliseconds(4000),
                         "G7: wall_ms=2000 returns within 4 s (took " + std::to_string(took.count()) + " ms)");
                AE_CHECK(took >= std::chrono::milliseconds(1500), "G7: ... and not before the deadline (took " + std::to_string(took.count()) + " ms)");
            }

            // G7-WASM cancel: wall_ms is far away (60 s); the session stop (ctx.cancellation) releases the call in < 1 s.
            {
                auto proxy = make_loopback_proxy();
                auto const text = fetch_with_through(bytes, proxy, peer.port(), 60000, "GET 0", std::chrono::milliseconds(300),
                                                     "G7-cancel", &took);
                std::cerr << "  (G7 cancel guest saw: " << text << ", took " << took.count() << " ms)\n";
                AE_CHECK(took < std::chrono::milliseconds(1300),
                         "G7: cancelling the call releases the parked import within 1 s of the stop (took " +
                             std::to_string(took.count()) + " ms)");
                AE_CHECK(text.find("name: \"network-error\"") != std::string::npos, "G7: the guest sees an http-error (cancel maps to network-error), not a hang");
            }
            check_admission_idle("G7-WASM");

            // G17-WASM (a): the request-size check in the import. wasmtime's own hostcall fuel traps a guest-to-host list copy of
            // ~4 MiB or more BEFORE the import runs (measured: 2 MiB passes, 4 MiB traps), so the host cap is exercised by
            // LOWERING it to 1 MiB (the cap is a host-set atomic, ADR-241 8.2) and sending 2 MiB: the import refuses the body
            // before the host-heap copy, so the injected backend's gate never runs and nothing connects.
            {
                struct CapRestore {
                    std::uint64_t old = sandbox::egress_limits().max_request_bytes.load();
                    ~CapRestore() { sandbox::egress_limits().max_request_bytes.store(old); }
                } restore;
                sandbox::egress_limits().max_request_bytes.store(1024u * 1024u);
                int const accepted_before = peer.accept_count();
                auto proxy = make_loopback_proxy();
                auto const text = fetch_with_through(bytes, proxy, peer.port(), 5000, "POST " + std::to_string(2u * 1024u * 1024u),
                                                     std::chrono::milliseconds(0), "G17-cap", &took);
                std::cerr << "  (G17 over-cap guest saw: " << text << ")\n";
                AE_CHECK(text.find("name: \"byte-cap-exceeded\"") != std::string::npos,
                         "G17: the guest sees byte-cap-exceeded for a body over the (lowered) request cap");
                AE_CHECK(proxy->gate_calls() == 0, "G17: the egress gate was never reached (refused before the body copy)");
                AE_CHECK(peer.accept_count() == accepted_before, "G17: nothing connected to the peer");
                // Not observable from here: whether the host heap grew past the WIT copy (no allocation hook); the
                // `gate_calls() == 0` row pins the refusal's position (before `reserve`/copy) instead.
            }
            // G17-WASM (b): the real 17 MiB body at the default 16 MiB cap. wasmtime's hostcall fuel stops it first: the
            // guest traps inside `http-request`, the host import never runs, nothing is gated or connected.
            {
                int const accepted_before = peer.accept_count();
                auto proxy = make_loopback_proxy();
                std::string call_error;
                auto const text = fetch_with_through(bytes, proxy, peer.port(), 5000, "POST " + std::to_string(17u * 1024u * 1024u),
                                                     std::chrono::milliseconds(0), "G17-17MiB", &took, &call_error);
                std::cerr << "  (G17 17 MiB: guest text '" << text << "', call error: " << call_error.substr(0, 60) << ")\n";
                AE_CHECK(!call_error.empty() || text.find("name: \"byte-cap-exceeded\"") != std::string::npos,
                         "G17: a 17 MiB body never succeeds: the call traps (hostcall fuel) or the guest sees byte-cap-exceeded");
                AE_CHECK(proxy->gate_calls() == 0, "G17: 17 MiB: the egress gate was never reached");
                AE_CHECK(peer.accept_count() == accepted_before, "G17: 17 MiB: nothing connected to the peer");
            }
            check_admission_idle("G17-WASM");
        }
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
