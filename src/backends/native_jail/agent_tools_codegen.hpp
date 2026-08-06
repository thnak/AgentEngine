#pragma once
// Implements 026-Agent-Facing-Runtime-Surface.md §4/§5 -- Milestone 3 Phase G1. `agent.tools` as
// ORDINARY Python callables (`tools.web_search(query="...", max_results=5)`), not the raw
// `call_tool("name", '{"query":"..."}')` bridge F2 built (010 §6): "Generated from the same tool
// metadata as everything else (006 §1)... so they cannot drift" (026 §4) -- made literal here by
// building each generated function's real signature/docstring straight from the SAME
// `ToolDescriptor` (core/tool_pipeline.hpp) every other tool-pipeline caller already reads, never a
// hand-authored per-tool wrapper.
//
// Pure C++, no Python.h dependency: schema -> Python-source-text generation is ordinary string
// building over `core/json_value.hpp`'s already-parsed `json::Value` (the SAME parser
// `Internal_call_tool` uses at call time, mediated_python_runner.cpp) -- there is no reason to make
// the GENERATED Python re-parse its own schema at runtime via Python's `json` module; the schema is
// already fully structured data on the C++ side, at `initialize()` time. `json` IS needed, and
// imported, in the generated module source itself -- but only to encode/decode the WIRE format
// `_ae_internal.call_tool` speaks (F2's raw-JSON-text boundary) at CALL time, not to re-derive the
// schema. This header stays Python-free specifically so its string-generation logic is unit-testable
// without an embedded CPython interpreter (tests/test_agent_tools_codegen.cpp); the actual `PyRun_
// String` wiring that executes the generated source lives in mediated_python_runner.cpp, alongside
// this header's own Python-execution counterpart, `run_agent_tools_bootstrap`.
//
// Unlike F2's raw `call_tool` bridge (which deliberately avoided `import json` -- see
// mediated_python_runner.cpp's `kMediationBootstrapSource` comment), the module source built HERE
// does `import json`, deliberately and safely: F2's bootstrap runs unconditionally for every session
// regardless of whether any tool is bridged, so importing `json` there would have widened every
// session's Layer-0 keep-set even when `agent.tools` never exists at all. This generator's output is
// only ever run when a `ToolBridgeConfig` is actually configured for the session (`MediatedPython
// Config::tool_bridge.has_value()`) -- i.e. only for a session that is ALREADY getting a real
// `agent.tools` module built from real, host-configured tools -- and each session is its own process
// (ADR-002 §5.5.6), so `json` becoming importable exactly when `agent.tools` exists is the intended
// shape, not a leak into a session that never asked for it.

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_pipeline.hpp"

namespace agentengine::native_jail {

// -- Identifier/keyword safety --------------------------------------------------------------
//
// A struct's field name is a valid C++ identifier by construction (the compiler already enforced
// that when AE_JSON_SCHEMA(...) was written), but "valid C++ identifier" and "valid, unreserved
// Python identifier" are NOT the same set: a handful of words (`from`, `in`, `is`, `pass`, `del`,
// `and`, `or`, `not`, ...) are ordinary C++ identifiers and reserved Python keywords at the same
// time. A field/tool name that collides with either rule cannot become a Python parameter/def name
// without a SyntaxError, so it disqualifies the WHOLE tool from being exposed via `agent.tools` --
// loud (a `result` error at generation time, a host-authorship problem to fix in the tool's own
// Args struct), never a silently dropped or silently mis-generated parameter.

[[nodiscard]] inline bool is_python_keyword(std::string const& s) {
    static std::unordered_set<std::string> const kKeywords = {
        "False",  "None",     "True",   "and",    "as",       "assert", "async",  "await",
        "break",  "class",    "continue", "def",  "del",      "elif",   "else",   "except",
        "finally", "for",     "from",   "global", "if",       "import", "in",     "is",
        "lambda", "nonlocal", "not",    "or",     "pass",     "raise",  "return", "try",
        "while",  "with",     "yield",
    };
    return kKeywords.contains(s);
}

[[nodiscard]] inline bool is_python_identifier(std::string const& s) {
    if (s.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_') return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return !is_python_keyword(s);
}

// Coarse, deliberately: json_schema.hpp's own `type_fragment<T>()` output only ever describes
// SHAPE, not a fully reconstructed nested type (e.g. a nested object's own field types don't
// recurse into the hint) -- matching 026 §5's own "guessable from its name" bar rather than chasing
// fidelity C++23 has no reflection to generate anyway (json_schema.hpp's header comment already
// names this same ceiling for default VALUES, which are equally unavailable here: only "required"
// vs "optional" is derivable, so an optional field's generated Python default is always `None`,
// never the field's real default-member-initializer value).
[[nodiscard]] inline std::string python_type_hint(json::Value const& type_fragment) {
    json::Value const* type_field = type_fragment.find("type");
    std::string const type_name = (type_field && type_field->is_string()) ? type_field->as_string() : "";
    if (type_name == "string") return "str";
    if (type_name == "integer") return "int";
    if (type_name == "number") return "float";
    if (type_name == "boolean") return "bool";
    if (type_name == "array") return "list";
    if (type_name == "object") return "dict";
    return "object";
}

// Backslash/double-quote escaped so the result is safe to embed inside a `"""..."""` docstring
// literal (three escaped quotes in a row can never form an unescaped closing triple-quote) --
// embedded newlines are left RAW, since triple-quoted strings support them natively and escaping
// them to a literal `\n` would make a multi-line description render as literal backslash-n text
// under `help()`, which is worse, not safer.
[[nodiscard]] inline std::string escape_for_docstring(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

struct ParsedToolParams {
    // "" for a zero-argument tool (a bare trailing `*` with nothing after it is a Python
    // SyntaxError), otherwise "*, name: hint[ = None], ..." -- keyword-only throughout (see
    // `generate_tool_function_source`'s own comment for why).
    std::string params;
    // Python statements (already newline-terminated) that assemble `_args` from the parameters
    // above, to be indented into the generated function's body.
    std::string body_build;
};

[[nodiscard]] inline result<ParsedToolParams> parse_tool_params(ToolDescriptor const& descriptor) {
    auto schema = json::parse(descriptor.args_schema_json);
    if (!schema) return std::unexpected(schema.error());

    json::Value const* properties = schema->find("properties");
    json::Value const* required_arr = schema->find("required");
    std::vector<std::string> required_names;
    if (required_arr && required_arr->is_array()) {
        for (auto const& r : required_arr->as_array()) {
            if (r.is_string()) required_names.push_back(r.as_string());
        }
    }
    auto is_required = [&](std::string const& name) {
        return std::find(required_names.begin(), required_names.end(), name) != required_names.end();
    };

    std::vector<std::string> param_strs;
    std::string body_build;
    if (properties && properties->is_object()) {
        for (auto const& [prop_name, prop_schema] : properties->as_object()) {
            if (!is_python_identifier(prop_name)) {
                return std::unexpected(error{failure_class::contract,
                    "tool '" + descriptor.name + "' has an argument field '" + prop_name + "' that "
                    "is not a usable Python parameter name (not an identifier, or a reserved "
                    "keyword) -- cannot generate agent.tools." + descriptor.name,
                    "python.tool_arg_name_not_identifier"});
            }
            std::string const hint = python_type_hint(prop_schema);
            if (is_required(prop_name)) {
                param_strs.push_back(prop_name + ": " + hint);
                body_build += "    _args['" + prop_name + "'] = " + prop_name + "\n";
            } else {
                param_strs.push_back(prop_name + ": " + hint + " = None");
                body_build += "    if " + prop_name + " is not None:\n        _args['" + prop_name +
                              "'] = " + prop_name + "\n";
            }
        }
    }

    ParsedToolParams parsed;
    if (!param_strs.empty()) {
        parsed.params = "*";
        for (auto const& p : param_strs) parsed.params += ", " + p;
    }
    parsed.body_build = std::move(body_build);
    return parsed;
}

// One tool -> one real Python function definition. Keyword-only parameters throughout (`*, name:
// hint, ...`) -- sidesteps Python's "no default parameter before a non-default one" ordering rule
// entirely, since a struct's field declaration order has no reason to already be required-first,
// and matches 026 §4's own calling example verbatim (`tools.web_search(query=..., max_results=5)`
// -- every argument already passed by keyword). Builds the wire-format dict from only the
// parameters actually supplied (an optional parameter left at its `None` default is omitted from
// `_args` entirely, mirroring `json_schema.hpp`'s own "absent, not null" rule for an unset optional
// field), JSON-encodes it, calls the F2 bridge, and decodes the reply into `_AeReply` -- an
// attribute-accessible generic wrapper, not a raw dict ("typed results... attribute access is
// guessable", 026 §4), and not a per-tool NOMINAL dataclass either: naming one real Python class per
// tool would need `exec()`-generated class bodies for genuinely no behavioral gain over one shared
// wrapper class, since neither approach can validate a reply's shape any more precisely without
// re-deriving `reply_schema_json` a second time at the attribute level -- named here as a residual,
// not a silently narrower claim than 026 §4's "dataclass-shaped" language.
[[nodiscard]] inline result<std::string> generate_tool_function_source(ToolDescriptor const& descriptor) {
    if (!is_python_identifier(descriptor.name)) {
        return std::unexpected(error{failure_class::contract,
            "tool name '" + descriptor.name + "' is not a usable Python identifier -- cannot "
            "generate agent.tools." + descriptor.name, "python.tool_name_not_identifier"});
    }
    auto parsed = parse_tool_params(descriptor);
    if (!parsed) return std::unexpected(parsed.error());

    std::string src;
    src += "def " + descriptor.name + "(" + parsed->params + "):\n";
    src += "    \"\"\"" + escape_for_docstring(descriptor.description) + "\"\"\"\n";
    src += "    _args = {}\n";
    src += parsed->body_build;
    src += "    _reply_json = _ae_internal.call_tool('" + descriptor.name + "', _json.dumps(_args))\n";
    src += "    return _AeReply(_json.loads(_reply_json))\n";
    return src;
}

// The full `agent.tools` module source: one shared `_AeReply` wrapper class, then one generated
// function per bridged tool, then real `agent`/`agent.tools` module objects (built from `type(sys)`
// -- no `import types` needed, `sys` is already Layer-0-permanent) registered into `sys.modules` so
// ordinary `import agent`/`from agent import tools` resolve them straight from that cache, never
// touching the meta-path finder at all (the same "never re-resolved fresh" property `os`/`socket`/
// `subprocess` already rely on -- mediated_python_runner.cpp's `kPinnedMediatedModules` comment).
[[nodiscard]] inline result<std::string> generate_agent_tools_module_source(
    std::vector<ToolDescriptor> const& bridged_tools) {
    std::string src =
        "import json as _json\n"
        "import sys as _sys\n"
        "\n"
        "class _AeReply:\n"
        "    def __init__(self, _data):\n"
        "        self.__dict__.update(_data)\n"
        "    def __repr__(self):\n"
        "        return 'Reply(' + repr(self.__dict__) + ')'\n"
        "\n";
    for (auto const& descriptor : bridged_tools) {
        auto fn_src = generate_tool_function_source(descriptor);
        if (!fn_src) return std::unexpected(fn_src.error());
        src += *fn_src;
        src += "\n";
    }
    src += "_ModuleType = type(_sys)\n";
    src += "_agent_module = _ModuleType('agent')\n";
    src += "_tools_module = _ModuleType('agent.tools')\n";
    for (auto const& descriptor : bridged_tools) {
        src += "_tools_module." + descriptor.name + " = " + descriptor.name + "\n";
    }
    src += "_agent_module.tools = _tools_module\n";
    src += "_sys.modules['agent'] = _agent_module\n";
    src += "_sys.modules['agent.tools'] = _tools_module\n";
    return src;
}

// 026 §4's ".pyi stub so the shape is inspectable" -- generated text only. There is no established
// consumer in this codebase yet (no LSP/static-analysis integration point exists to write this to),
// so wiring it to a discoverable path is left a named residual rather than an invented delivery
// mechanism nothing asked for; `dir()`/`help()` on the REAL module/function objects (already
// satisfied by `generate_agent_tools_module_source`'s real `def`s) covers runtime introspection
// without this stub at all -- this text is for tooling that inspects the agent's code WITHOUT
// running the interpreter (mypy, an IDE), a genuinely separate consumer.
[[nodiscard]] inline result<std::string> generate_agent_tools_pyi_stub(
    std::vector<ToolDescriptor> const& bridged_tools) {
    std::string src = "class Reply:\n    def __getattr__(self, name: str) -> object: ...\n\n";
    for (auto const& descriptor : bridged_tools) {
        if (!is_python_identifier(descriptor.name)) {
            return std::unexpected(error{failure_class::contract,
                "tool name '" + descriptor.name + "' is not a usable Python identifier",
                "python.tool_name_not_identifier"});
        }
        auto parsed = parse_tool_params(descriptor);
        if (!parsed) return std::unexpected(parsed.error());
        src += "def " + descriptor.name + "(" + parsed->params + ") -> Reply: ...\n";
    }
    return src;
}

}  // namespace agentengine::native_jail
