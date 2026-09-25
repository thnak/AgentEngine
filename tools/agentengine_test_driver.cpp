// Implements ADR-182 (decisions/ADR-182-agent-test-driver-mcp.md): `agentengine_test_driver`, an MCP
// server over stdio (newline-delimited JSON-RPC) that drives in-process AgentEngine sessions with a
// scripted model. Everything but the stdin/stdout pump lives in tools/test_driver/test_driver.hpp.
//
// A host, like cli_chat: the engine library still binds nothing (ADR-039/061). stdio only; no port.
//
// The protocol stream is the ORIGINAL stdout, duplicated at startup; fd 1 is then pointed at stderr,
// so a stray printf anywhere in the engine can never corrupt the JSON-RPC stream (ADR-182 §12).
//
// Usage (e.g. in an MCP client config):
//   agentengine_test_driver                         scripted fixtures only
//   agentengine_test_driver --allow-live --live-key-file deep-seek.txt [--live-model deepseek-flash]
//       [--live-host api.deepseek.com] [--live-path-prefix /v1] [--live-max-calls 40]
//       [--record-dir build/test-driver-recordings]
//   --scenarios-root <dir>   where scenario_export writes / scenario_replay reads (default tests/scenarios)
// Every flag is a HOST decision, fixed for the process: no tool argument can turn live mode on, pick
// the key, or change the budget (ADR-182 §12 C-3). Live mode needs an AGENTENGINE_WITH_HTTPS build.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "test_driver/test_driver.hpp"
#ifdef AGENTENGINE_WITH_HTTPS
#include "test_driver/live_backend.hpp"
#endif

namespace {

// Returns a FILE* on a duplicate of the real stdout, then redirects fd 1 to stderr.
std::FILE* take_protocol_stdout() {
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    int const dup_fd = _dup(_fileno(stdout));
    if (dup_fd < 0) return nullptr;
    (void)_setmode(dup_fd, _O_BINARY);
    (void)_dup2(_fileno(stderr), _fileno(stdout));
    return _fdopen(dup_fd, "wb");
#else
    int const dup_fd = dup(fileno(stdout));
    if (dup_fd < 0) return nullptr;
    (void)dup2(fileno(stderr), fileno(stdout));
    return fdopen(dup_fd, "w");
#endif
}

#ifdef AGENTENGINE_WITH_HTTPS
// First line of the key file, trimmed of whitespace. Never echoed anywhere.
std::string read_key_file(std::string const& path) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    auto const is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!line.empty() && is_space(line.back())) line.pop_back();
    std::size_t start = 0;
    while (start < line.size() && is_space(line[start])) ++start;
    return line.substr(start);
}
#endif

struct Args {
    bool        allow_live = false;
    std::string key_file;
    std::string host = "api.deepseek.com";
    std::string path_prefix = "/v1";
    std::string model = "deepseek-flash";
    std::string record_dir;
    unsigned    max_calls = 40;
    std::string scenarios_root = "tests/scenarios";
};

bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string_view const f = argv[i];
        auto value = [&](std::string& into) {
            if (i + 1 >= argc) return false;
            into = argv[++i];
            return true;
        };
        if (f == "--allow-live") {
            a.allow_live = true;
        } else if (f == "--live-key-file") {
            if (!value(a.key_file)) return false;
        } else if (f == "--live-host") {
            if (!value(a.host)) return false;
        } else if (f == "--live-path-prefix") {
            if (!value(a.path_prefix)) return false;
        } else if (f == "--live-model") {
            if (!value(a.model)) return false;
        } else if (f == "--record-dir") {
            if (!value(a.record_dir)) return false;
        } else if (f == "--scenarios-root") {
            if (!value(a.scenarios_root)) return false;
        } else if (f == "--live-max-calls") {
            std::string n;
            if (!value(n)) return false;
            a.max_calls = static_cast<unsigned>(std::strtoul(n.c_str(), nullptr, 10));
        } else {
            std::fprintf(stderr, "agentengine_test_driver: unknown flag %s\n", std::string(f).c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 2;

    agentengine::test_driver::DriverConfig config;
    config.scenarios_root = args.scenarios_root;
    if (args.allow_live) {
#ifdef AGENTENGINE_WITH_HTTPS
        std::string key = args.key_file.empty() ? std::string{} : read_key_file(args.key_file);
        if (key.empty()) {
            std::fprintf(stderr, "agentengine_test_driver: --allow-live needs a readable --live-key-file\n");
            return 2;
        }
        agentengine::test_driver::LiveConfig live;
        live.host = args.host;
        live.path_prefix = args.path_prefix;
        live.model = args.model;
        live.key = key;
        live.record_dir = args.record_dir;
        live.max_calls = args.max_calls;
        config.secret_canaries.push_back(key);
        config.live_description = args.model + " @ " + args.host + " (max " + std::to_string(args.max_calls) +
                                  " calls/session" + (args.record_dir.empty() ? "" : ", recorded") + ")";
        config.live_backend_factory = [live](std::string const& session_id)
            -> std::shared_ptr<agentengine::test_driver::ModelBackend> {
            return std::make_shared<agentengine::test_driver::LiveBackend>(live, session_id);
        };
#else
        std::fprintf(stderr, "agentengine_test_driver: --allow-live needs an AGENTENGINE_WITH_HTTPS build\n");
        return 2;
#endif
    }

    std::FILE* out = take_protocol_stdout();
    if (out == nullptr) {
        std::fprintf(stderr, "agentengine_test_driver: could not duplicate stdout\n");
        return 2;
    }
    std::fprintf(stderr, "agentengine_test_driver %s ready (stdio, MCP %s, live: %s)\n",
                 std::string(agentengine::test_driver::kServerVersion).c_str(),
                 std::string(agentengine::test_driver::kProtocolVersion).c_str(),
                 config.live_backend_factory ? config.live_description.c_str() : "off");

    agentengine::test_driver::Driver driver(std::move(config));
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::optional<std::string> reply = driver.handle_line(line);
        if (!reply) continue;
        reply->push_back('\n');
        std::fwrite(reply->data(), 1, reply->size(), out);
        std::fflush(out);
    }
    // stdin EOF: the client went away. Close every session (each pool drains its jobs; phase 1 has
    // only in-process test tools, so this always completes).
    driver.close_all();
    std::fclose(out);
    return 0;
}
