// Implements ADR-182 (decisions/ADR-182-agent-test-driver-mcp.md): `agentengine_test_driver`, an MCP
// server over stdio (newline-delimited JSON-RPC) that drives in-process AgentEngine sessions with a
// scripted model. Everything but the stdin/stdout pump lives in tools/test_driver/test_driver.hpp.
//
// A host, like cli_chat: the engine library still binds nothing (ADR-039/061). stdio only; no port.
//
// The protocol stream is the ORIGINAL stdout, duplicated at startup; fd 1 is then pointed at stderr,
// so a stray printf anywhere in the engine can never corrupt the JSON-RPC stream (ADR-182 §12).
//
// Usage (e.g. in an MCP client config):  agentengine_test_driver
// No flags in phase 1: fixtures and tools are compiled in (ADR-182 §12 C-3).

#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "test_driver/test_driver.hpp"

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

}  // namespace

int main() {
    std::FILE* out = take_protocol_stdout();
    if (out == nullptr) {
        std::fprintf(stderr, "agentengine_test_driver: could not duplicate stdout\n");
        return 2;
    }
    std::fprintf(stderr, "agentengine_test_driver %s ready (stdio, MCP %s)\n",
                 std::string(agentengine::test_driver::kServerVersion).c_str(),
                 std::string(agentengine::test_driver::kProtocolVersion).c_str());

    agentengine::test_driver::Driver driver;
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
