// Implements ADR-182 §16 (decisions/ADR-182-agent-test-driver-mcp.md): replays checked-in scenarios
// (tests/scenarios/*.json) with the scripted model and diffs the normalized event stream. No network,
// no Claude: this is what turns a test agent's exploration -- including a live-model run -- into a
// deterministic ctest (022 §3 golden traces).
//
// Usage: agentengine_scenario_runner [--fixtures-root <dir>] [--stamp-requests | --restamp-requests]
//                                    <scenario.json>...
// --fixtures-root: where a scenario's file fixture (ADR-182 §21) is found. No git check here: the
// runner is run by a person or CI, not by the tester.
// Exit code: 0 if every scenario passes, 1 otherwise. A scenario whose turns carry no request digest
// fails (§22): its model requests would go unchecked.
//
// --stamp-requests (ADR-182 §19, C8) is maintenance for a scenario exported before request digests
// existed: it replays the scenario and, only if everything else in the replay passes and it made exactly
// one model call per recorded turn, writes each call's request digest into its turn. It never changes a
// turn that already has a digest. --restamp-requests first removes every digest, for when the digest
// itself changes (§22). Neither applies to a forked scenario (format 2): re-export it instead.

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

#include "test_driver/test_driver.hpp"

namespace {

namespace td = agentengine::test_driver;
using agentengine::json::Value;

// The scenario with every model turn's request_digest removed.
Value without_digests(Value const& scenario) {
    td::Members top;
    for (auto const& [k, v] : scenario.as_object()) {
        if (k != "model_turns" || !v.is_array()) {
            top.emplace_back(k, v);
            continue;
        }
        std::vector<Value> turns;
        for (Value const& t : v.as_array()) {
            td::Members m;
            for (auto const& [tk, tv] : t.as_object())
                if (tk != "request_digest") m.emplace_back(tk, tv);
            turns.push_back(td::obj(std::move(m)));
        }
        top.emplace_back(k, td::arr(std::move(turns)));
    }
    return td::obj(std::move(top));
}

// Returns an explanation when the scenario was not stamped, empty when it was (or had nothing to do).
std::string stamp(Value& scenario, td::ReplayReport const& report) {
    if (Value const* segs = scenario.find("segments"); segs != nullptr && segs->is_array() && !segs->as_array().empty()) {
        return "a forked scenario is not stamped; re-export it";
    }
    // The only acceptable problem is the missing digests themselves.
    bool const only_missing = report.problems.size() == 1 && report.turns_without_digest != 0;
    if (!report.passed && !only_missing) return "the replay failed, so its requests are not trusted";
    Value const* turns = scenario.find("model_turns");
    if (turns == nullptr || !turns->is_array()) return {};
    std::vector<Value> const& old_turns = turns->as_array();
    if (old_turns.size() != report.observed_request_digests.size()) {
        return "the replay made " + std::to_string(report.observed_request_digests.size()) + " model calls for " +
               std::to_string(old_turns.size()) + " recorded turns";
    }
    std::vector<Value> new_turns;
    for (std::size_t i = 0; i < old_turns.size(); ++i) {
        td::Members m;
        bool had = false;
        for (auto const& [k, v] : old_turns[i].as_object()) {
            if (k == "request_digest") had = true;
            m.emplace_back(k, v);
        }
        if (!had) m.emplace_back("request_digest", td::str(report.observed_request_digests[i]));
        new_turns.push_back(td::obj(std::move(m)));
    }
    td::Members top;
    for (auto const& [k, v] : scenario.as_object()) {
        top.emplace_back(k, k == "model_turns" ? td::arr(new_turns) : v);
    }
    scenario = td::obj(std::move(top));
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    int first = 1;
    td::ReplayFixtures fixtures;
    bool stamping = false;
    bool restamping = false;
    while (first < argc) {
        std::string_view const f = argv[first];
        if (f == "--stamp-requests" || f == "--restamp-requests") {
            stamping = true;
            restamping = f == "--restamp-requests";
            ++first;
        } else if (f == "--fixtures-root" && first + 1 < argc) {
            fixtures.root = argv[first + 1];
            first += 2;
        } else {
            break;
        }
    }
    if (argc <= first) {
        std::fprintf(stderr,
                     "usage: agentengine_scenario_runner [--fixtures-root <dir>] [--stamp-requests | "
                     "--restamp-requests] <scenario.json>...\n");
        return 2;
    }
    int failed = 0;
    for (int i = first; i < argc; ++i) {
        std::string const path = argv[i];
        auto scenario = td::read_scenario_file(path);
        if (!scenario) {
            std::printf("FAIL %s\n  %s\n", path.c_str(), scenario.error().message.c_str());
            ++failed;
            continue;
        }
        Value const input = restamping ? without_digests(*scenario) : *scenario;
        td::ReplayReport const report = td::replay_scenario(input, fixtures);
        std::printf("%s %s (%zu events compared, %zu model requests checked)\n", report.passed ? "PASS" : "FAIL",
                    path.c_str(), report.events_compared, report.requests_checked);
        for (std::string const& p : report.problems) std::printf("  %s\n", p.c_str());
        if (!stamping) {
            if (!report.passed) ++failed;
            continue;
        }
        Value stamped = input;
        if (std::string const why = stamp(stamped, report); !why.empty()) {
            std::printf("  not stamped: %s\n", why.c_str());
            ++failed;
            continue;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::printf("  not stamped: cannot write %s\n", path.c_str());
            ++failed;
            continue;
        }
        out << agentengine::json::dump(stamped) << "\n";
        std::printf("  stamped %zu request digest(s)\n", report.observed_request_digests.size());
    }
    return failed == 0 ? 0 : 1;
}
