# tests/core/json/: core JSON/YAML values and JSON Schema (core/json_*.hpp, core/yaml_value.hpp).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "core/json" ctest label for every test here (cmake/AgentEngineTests.cmake).

if(AGENTENGINE_WITH_HTTPS)
  # ---- Described<T, "..."> (core/json_schema.hpp): per-parameter tool descriptions, proven to
  # survive both OpenAI's and Anthropic's real translate_tool() end to end.
  agentengine_add_test(test_json_schema_described
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings)
endif()

# ---- Gap-3 fix (2026-08-14, decisions/ADR-045-*.md) -- core/json_schema_validator.hpp, proving
# 006 §8 G2 (schema violation -> structured error) and 015 §7 G2 (cyclic $ref rejected). No HTTPS
# dependency (unlike its neighbors above) -- deliberately outside that conditional block.
agentengine_add_test(test_json_schema_validator LIBS agentengine::core agentengine_warnings)

# ---- Milestone 7 Phase F1 -- 015 §2/§3's YAML-subset parser (core\yaml_value.hpp): block/flow
# style, block literals, quoted/plain scalars -- proven against the RFC's own example documents ----
agentengine_add_test(test_yaml_value LIBS agentengine::core agentengine_warnings)

# ---- Dependency-free JSON value/parser/serializer (M2 Phase B task B2, core/json_value.hpp) ----
# Header-only, core-tier -- links only agentengine::core (no nlohmann::json; this IS the seam that
# keeps nlohmann out of product code, see CONVENTIONS.md dependency-tier discipline).
# json::dump string escaping, appending unescaped runs: byte-identical to the per-byte writer it replaced.
agentengine_add_test(test_json_dump_escape LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_json_value LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_json_parse_allocation LIBS agentengine::core agentengine_warnings)

# ---- Tool JSON-Schema derivation (M2 Phase B task B1, 006 §1) ----------------------------------
# Header-only (core/json_schema.hpp, core/tool.hpp); nlohmann_json is test-only here too (parses
# the emitted schema string to assert real JSON structure, not substring-match it).
agentengine_add_test(test_tool_json_schema
  LIBS agentengine::core agentengine_warnings nlohmann_json::nlohmann_json)

# ---- Tool JSON codec: to_json/from_json (M2 Phase B task B2, 006 §3 steps 2/9) ------------------
# Header-only (core/json_schema.hpp's AE_JSON_SCHEMA-generated ae_to_json/ae_from_json), core-tier
# -- links only agentengine::core (no nlohmann::json; exercises the dependency-free codec itself).
agentengine_add_test(test_json_schema_codec LIBS agentengine::core agentengine_warnings)
