#pragma once
// Shared reference fixture for M2 Phase F task F2 (007 §9 gate G6): the small, explicit "reference
// policy set" both tools/policy_reachability.cpp (the CI tool) and
// tests/test_policy_reachability.cpp (its proof) enumerate against, so the two can never drift
// apart. See include/agentengine/trust/policy_reachability.hpp's file-top comment for why this walks
// the mechanical CapabilitySet::contains() surface rather than a 007 §5 declarative rule set (none
// exists yet, decision 4).
//
// `build_reference_fixture()` is the CLEAN set -- four fixture tools (one capability kind each) and
// four fixture agents, all pairings correct, zero findings expected:
//   - echo-agent-registered: built through the REAL register_agent<EchoAgent>() path (Phase E's own
//     compiler) -- proves this enumerator's decisions agree with what registration itself already
//     decided for at least one real, full round trip, not only synthetic ReachabilityAgent structs.
//   - reader-agent-covering / reader-agent-too-narrow: the same tool (ReaderTool, FsRead<"data">)
//     against two different hand-built ceilings -- one that covers it (GRANTED) and one that
//     doesn't (DENIED, still zero findings since the oracle correctly predicts DENIED) -- proving
//     the enumerator reaches both outcomes, not just the happy path.
//   - broad-agent: four more tools/kinds (fs_write, secret, net_out, clock) together, ceiling
//     covering all four -- breadth across capability kinds beyond fs_read/entropy.
//
// `add_over_broad_positive_control()` is a separate, additive function (not part of the clean set --
// see its own comment below) supplying the exit criterion's own demanded positive control
// (docs/issues/m2-phase-f-adr-track-tasks.md: "catching... at least one class of over-broad
// reachability a manual review would miss"): an agent whose ceiling grants NetOut but whose only
// declared tool (EchoTool, Entropy-only) never asks for it. A reviewer checking "does EchoTool's
// requirement fit inside the ceiling?" sees PASS and stops there; the ceiling's own unused NetOut
// grant is invisible unless something specifically diffs granted-kinds against required-kinds, which
// is exactly what enumerate_policy_reachability's over_broad_grant finding does.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/trust/policy_reachability.hpp"

namespace agentengine::policy_reachability_fixture {

// -- fixture tools, one per capability kind exercised, each the minimal real Tool<> declaration ----

struct EchoArgs {
    std::string message;
};
AE_JSON_SCHEMA(EchoArgs, message)
struct EchoReply {
    std::string echoed;
};
AE_JSON_SCHEMA(EchoReply, echoed)
struct EchoTool : Tool<EchoTool, Capabilities<cap::decl::Entropy>> {
    static constexpr std::string_view name = "echo";
    static constexpr std::string_view description = "Echo the input message back.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    static result<Reply> invoke(Args args, EffectContext&) { return Reply{"echo: " + args.message}; }
};

struct ReaderArgs {
    std::string path;
};
AE_JSON_SCHEMA(ReaderArgs, path)
struct ReaderReply {
    std::string contents;
};
AE_JSON_SCHEMA(ReaderReply, contents)
struct ReaderTool : Tool<ReaderTool, Capabilities<cap::decl::FsRead<"data">>> {
    static constexpr std::string_view name = "read-data";
    static constexpr std::string_view description = "Read a file under the data mount.";
    using Args = ReaderArgs;
    using Reply = ReaderReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{"stub"}; }
};

struct WriterArgs {
    std::string path;
    std::string contents;
};
AE_JSON_SCHEMA(WriterArgs, path, contents)
struct WriterReply {
    bool ok = false;
};
AE_JSON_SCHEMA(WriterReply, ok)
struct WriterTool : Tool<WriterTool, Capabilities<cap::decl::FsWrite<"scratch">>> {
    static constexpr std::string_view name = "write-scratch";
    static constexpr std::string_view description = "Write a file under the scratch mount.";
    using Args = WriterArgs;
    using Reply = WriterReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{true}; }
};

struct SecretArgs {
    std::string name;
};
AE_JSON_SCHEMA(SecretArgs, name)
struct SecretReply {
    std::string value;
};
AE_JSON_SCHEMA(SecretReply, value)
struct SecretTool : Tool<SecretTool, Capabilities<cap::decl::Secret<"api-key">>> {
    static constexpr std::string_view name = "read-secret";
    static constexpr std::string_view description = "Resolve the api-key secret.";
    using Args = SecretArgs;
    using Reply = SecretReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{"stub"}; }
};

struct FetcherArgs {
    std::string path;
};
AE_JSON_SCHEMA(FetcherArgs, path)
struct FetcherReply {
    std::int64_t status = 0;
};
AE_JSON_SCHEMA(FetcherReply, status)
struct FetcherTool : Tool<FetcherTool, Capabilities<cap::decl::NetOut<"api.example.com">>> {
    static constexpr std::string_view name = "fetch-example";
    static constexpr std::string_view description = "Fetch from api.example.com.";
    using Args = FetcherArgs;
    using Reply = FetcherReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{200}; }
};

struct ClockArgs {
    std::string format;
};
AE_JSON_SCHEMA(ClockArgs, format)
struct ClockReply {
    std::int64_t millis = 0;
};
AE_JSON_SCHEMA(ClockReply, millis)
struct ClockTool : Tool<ClockTool, Capabilities<cap::decl::Clock<0>>> {
    static constexpr std::string_view name = "now";
    static constexpr std::string_view description = "Read the current time.";
    using Args = ClockArgs;
    using Reply = ClockReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{0}; }
};

// -- one fixture agent registered through the real path, for round-trip parity -------------------

struct EchoAgent : Agent<EchoAgent, ChatClientId<"anthropic:claude-opus-5">, Tools<EchoTool>,
                          Capabilities<cap::decl::Entropy>> {
    static constexpr std::string_view name = "echo-agent";
    static constexpr std::string_view instructions = "Declares Tools<EchoTool> and a covering ceiling.";
};

// -- the reference set builder ---------------------------------------------------------------------

inline void build_reference_fixture(std::vector<trust::ReachabilityAgent>& agents,
                                     std::vector<trust::ReachabilityOracleEntry>& oracle) {
    // echo-agent-registered: the real register_agent<A>() path. Registration itself is already
    // exercised elsewhere (tests/test_agent_tool_invocation.cpp); a failure here would mean this
    // fixture file itself regressed, not something F2 is meant to catch -- fail loudly rather than
    // silently skip the fixture.
    {
        auto meta = register_agent<EchoAgent>();
        if (!meta) {
            std::fprintf(stderr, "policy_reachability_fixture: EchoAgent failed to register: %s\n",
                         meta.error().message.c_str());
            std::abort();
        }
        agents.push_back(trust::ReachabilityAgent{"echo-agent-registered", meta->capability_ceiling,
                                                    meta->tools.descriptors()});
        oracle.push_back({"echo-agent-registered", "echo", capability_kind::entropy, true});
    }

    // reader-agent-covering: hand-built ceiling that covers ReaderTool's own real requirement.
    {
        std::vector<ToolDescriptor> tools{make_tool_descriptor<ReaderTool>()};
        std::vector<Capability> ceiling{cap::FsRead{"data", "", std::nullopt}};
        agents.push_back(trust::ReachabilityAgent{"reader-agent-covering", ceiling, tools});
        oracle.push_back({"reader-agent-covering", "read-data", capability_kind::fs_read, true});
    }

    // reader-agent-too-narrow: same tool, an empty ceiling -- proves the DENY path.
    {
        std::vector<ToolDescriptor> tools{make_tool_descriptor<ReaderTool>()};
        std::vector<Capability> ceiling{};
        agents.push_back(trust::ReachabilityAgent{"reader-agent-too-narrow", ceiling, tools});
        oracle.push_back({"reader-agent-too-narrow", "read-data", capability_kind::fs_read, false});
    }

    // broad-agent: four more kinds (fs_write, secret, net_out, clock), all covered.
    {
        std::vector<ToolDescriptor> tools{make_tool_descriptor<WriterTool>(), make_tool_descriptor<SecretTool>(),
                                           make_tool_descriptor<FetcherTool>(), make_tool_descriptor<ClockTool>()};
        std::vector<Capability> ceiling{
            cap::FsWrite{"scratch", "", std::nullopt, std::nullopt},
            cap::Secret{"api-key", std::chrono::seconds{0}},
            cap::NetOut{{"api.example.com"}, std::nullopt, {}},
            cap::Clock{std::chrono::milliseconds{0}},
        };
        agents.push_back(trust::ReachabilityAgent{"broad-agent", ceiling, tools});
        oracle.push_back({"broad-agent", "write-scratch", capability_kind::fs_write, true});
        oracle.push_back({"broad-agent", "read-secret", capability_kind::secret, true});
        oracle.push_back({"broad-agent", "fetch-example", capability_kind::net_out, true});
        oracle.push_back({"broad-agent", "now", capability_kind::clock, true});
    }

}

// over-broad-agent: EchoTool (Entropy-only) plus an UNUSED NetOut grant -- the exit criterion's own
// demanded positive control (see file-top comment). Deliberately NOT part of
// `build_reference_fixture()`'s own set: that set is what tools/policy_reachability.cpp runs as a CI
// gate and expects to pass cleanly (007 §10 Q3: findings are "for an operator to review", not a
// permanently-red gate over a known, accepted fixture entry) -- the detection capability itself is
// proven separately, by test_policy_reachability.cpp calling this function to append the positive
// control on top of the clean set and asserting the finding fires exactly as expected.
inline void add_over_broad_positive_control(std::vector<trust::ReachabilityAgent>& agents,
                                             std::vector<trust::ReachabilityOracleEntry>& oracle) {
    std::vector<ToolDescriptor> tools{make_tool_descriptor<EchoTool>()};
    std::vector<Capability> ceiling{cap::Entropy{}, cap::NetOut{{"unused.example.com"}, std::nullopt, {}}};
    agents.push_back(trust::ReachabilityAgent{"over-broad-agent", ceiling, tools});
    oracle.push_back({"over-broad-agent", "echo", capability_kind::entropy, true});
}

}  // namespace agentengine::policy_reachability_fixture
