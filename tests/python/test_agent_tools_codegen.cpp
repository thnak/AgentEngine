// Proof for Milestone 3 Phase G1
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// `src/backends/native_jail/agent_tools_codegen.hpp`'s schema-to-Python-source generation, in
// isolation from any embedded CPython interpreter (pure string building over the SAME
// `ToolDescriptor` metadata every other tool-pipeline caller reads). The generated source actually
// RUNNING under a real interpreter -- `from agent import tools; tools.echo_tool(message="hi")`,
// dir()/help(), a capability-denied call -- is proven separately in the Python-gated
// tests/test_mediated_python_runner_agent_tools.cpp; this file is about the TEXT being correct
// Python and shaped the way 026 §4 describes.
//
// ToolDescriptors are built directly (not via Tool<>/AE_JSON_SCHEMA) so this file can construct
// deliberately awkward schemas (zero properties, a Python-keyword-named field) without needing a
// real C++ struct for each -- schema JSON text is exactly what the codegen header itself consumes.

#include <cstdio>
#include <string>

#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "backends/native_jail/agent_tools_codegen.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stdout, "  ok: %s\n", what);
    }
}

agentengine::ToolDescriptor make_descriptor(std::string name, std::string description,
                                             std::string args_schema_json) {
    agentengine::ToolDescriptor d;
    d.name = std::move(name);
    d.description = std::move(description);
    d.args_schema_json = std::move(args_schema_json);
    d.reply_schema_json = R"({"type":"object","properties":{}})";
    d.invoke = [](agentengine::json::Value const&, agentengine::EffectContext&)
        -> agentengine::result<agentengine::json::Value> { return agentengine::json::Value::make_null(); };
    return d;
}

}  // namespace

int main() {
    using namespace agentengine::native_jail;

    // ---- G1-C1: real signature -- required field positional-by-keyword, optional field defaults to
    // None, keyword-only marker present, real type hints from the schema.
    {
        auto d = make_descriptor("web_search", R"(Searches the web for "query" and returns a summary.)",
                                  R"({"type":"object","properties":{"query":{"type":"string"},)"
                                  R"("max_results":{"type":"integer"}},"required":["query"]})");
        auto src = generate_tool_function_source(d);
        check(src.has_value(), "G1-C1: setup -- web_search's function source generates without error");
        if (src) {
            check(src->find("def web_search(*, query: str, max_results: int = None):") != std::string::npos,
                  "G1-C1: the generated signature has a real name, keyword-only marker, required "
                  "field with no default, optional field defaulting to None, and real type hints");
            check(src->find("_args['query'] = query") != std::string::npos,
                  "G1-C1: the required field is unconditionally included in the wire-format dict");
            check(src->find("if max_results is not None:") != std::string::npos,
                  "G1-C1: the optional field is only included when the caller actually passed it");
            check(src->find("_ae_internal.call_tool('web_search'") != std::string::npos,
                  "G1-C1: the generated body calls through F2's own bridge, by the tool's real name");
            check(src->find("\\\"query\\\"") != std::string::npos,
                  "G1-C2: an embedded double quote in the description is escaped, not left to break "
                  "the triple-quoted docstring");
        }
    }

    // ---- G1-C3 (negative control / boundary): a zero-argument tool gets a bare `def name():`, not
    // a syntactically invalid trailing `*` with nothing after it.
    {
        auto d = make_descriptor("ping", "Zero-argument tool.", R"({"type":"object","properties":{}})");
        auto src = generate_tool_function_source(d);
        check(src.has_value() && src->find("def ping():") != std::string::npos,
              "G1-C3: a zero-argument tool generates a bare empty parameter list, never a bare '*'");
    }

    // ---- G1-C4: a Python-keyword-named argument field disqualifies the tool, loudly (a result
    // error), never a silently broken or silently dropped parameter. ("from" is a perfectly
    // ordinary C++ identifier and a reserved Python keyword at the same time.)
    {
        auto d = make_descriptor("bad_arg_tool", "Has a Python-keyword-named argument.",
                                  R"({"type":"object","properties":{"from":{"type":"string"}},)"
                                  R"("required":["from"]})");
        auto src = generate_tool_function_source(d);
        check(!src.has_value() && src.error().code == "python.tool_arg_name_not_identifier",
              "G1-C4: a Python-keyword-named argument field fails generation loudly, not silently");
    }

    // ---- G1-C5: a tool name that isn't a valid Python identifier fails generation loudly too.
    {
        auto d = make_descriptor("not-an-identifier", "Bad name.", R"({"type":"object","properties":{}})");
        auto src = generate_tool_function_source(d);
        check(!src.has_value() && src.error().code == "python.tool_name_not_identifier",
              "G1-C5: a non-identifier tool name fails generation loudly, not silently");
    }

    // ---- G1-M1: the full module source registers real `agent`/`agent.tools` module objects into
    // sys.modules (so `from agent import tools` resolves via the cache, never the meta-path finder),
    // imports json (this generator's own deliberate, session-scoped exception to F2's "no new
    // stdlib import" rule), and attaches every generated function as a real module attribute.
    {
        std::vector<agentengine::ToolDescriptor> table;
        table.push_back(make_descriptor("web_search", "Search.",
                                         R"({"type":"object","properties":{"query":{"type":"string"}},)"
                                         R"("required":["query"]})"));
        table.push_back(make_descriptor("ping", "Ping.", R"({"type":"object","properties":{}})"));
        auto src = generate_agent_tools_module_source(table);
        check(src.has_value(), "G1-M1: setup -- the full module source generates without error");
        if (src) {
            check(src->find("import json as _json") != std::string::npos,
                  "G1-M1: the module source imports json for wire encoding");
            check(src->find("class _AeReply:") != std::string::npos,
                  "G1-M1: a shared attribute-accessible reply wrapper is defined once");
            check(src->find("def web_search(") != std::string::npos &&
                      src->find("def ping(") != std::string::npos,
                  "G1-M1: every bridged tool gets its own generated function");
            check(src->find("_tools_module.web_search = web_search") != std::string::npos &&
                      src->find("_tools_module.ping = ping") != std::string::npos,
                  "G1-M1: every generated function is attached to the tools module as a real attribute");
            check(src->find("_sys.modules['agent'] = _agent_module") != std::string::npos &&
                      src->find("_sys.modules['agent.tools'] = _tools_module") != std::string::npos,
                  "G1-M1: both agent and agent.tools are registered into sys.modules directly");
        }
    }

    // ---- G1-P1: the .pyi stub mirrors the real signatures (026 §4's separate introspection
    // deliverable, for tooling that inspects the agent's code without running the interpreter).
    {
        std::vector<agentengine::ToolDescriptor> table;
        table.push_back(make_descriptor("web_search", "Search.",
                                         R"({"type":"object","properties":{"query":{"type":"string"},)"
                                         R"("max_results":{"type":"integer"}},"required":["query"]})"));
        auto stub = generate_agent_tools_pyi_stub(table);
        check(stub.has_value(), "G1-P1: setup -- the .pyi stub generates without error");
        if (stub) {
            check(stub->find("def web_search(*, query: str, max_results: int = None) -> Reply: ...") !=
                      std::string::npos,
                  "G1-P1: the stub's signature matches the real generated function's signature exactly");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stdout, "test_agent_tools_codegen: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_agent_tools_codegen: %d check(s) failed\n", g_failures);
    return 1;
}
