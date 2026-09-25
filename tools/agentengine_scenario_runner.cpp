// Implements ADR-182 §16 (decisions/ADR-182-agent-test-driver-mcp.md): replays checked-in scenarios
// (tests/scenarios/*.json) with the scripted model and diffs the normalized event stream. No network,
// no Claude: this is what turns a test agent's exploration -- including a live-model run -- into a
// deterministic ctest (022 §3 golden traces).
//
// Usage: agentengine_scenario_runner <scenario.json>...
// Exit code: 0 if every scenario passes, 1 otherwise.

#include <cstdio>
#include <string>

#include "test_driver/test_driver.hpp"

int main(int argc, char** argv) {
    namespace td = agentengine::test_driver;
    if (argc < 2) {
        std::fprintf(stderr, "usage: agentengine_scenario_runner <scenario.json>...\n");
        return 2;
    }
    int failed = 0;
    for (int i = 1; i < argc; ++i) {
        std::string const path = argv[i];
        auto scenario = td::read_scenario_file(path);
        if (!scenario) {
            std::printf("FAIL %s\n  %s\n", path.c_str(), scenario.error().message.c_str());
            ++failed;
            continue;
        }
        td::ReplayReport const report = td::replay_scenario(*scenario);
        std::printf("%s %s (%zu events compared)\n", report.passed ? "PASS" : "FAIL", path.c_str(),
                    report.events_compared);
        for (std::string const& p : report.problems) std::printf("  %s\n", p.c_str());
        if (!report.passed) ++failed;
    }
    return failed == 0 ? 0 : 1;
}
