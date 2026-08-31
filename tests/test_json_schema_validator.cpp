// Gap-3 fix (2026-08-14, decisions/ADR-045-*.md): proves core/json_schema_validator.hpp against its
// two governing gates -- 006 SS8 G2 ("a negative suite including a schema violation produces a
// structured [error]") and 015 SS7 G2 ("a negative corpus including cyclic $ref is rejected with
// precise diagnostics") -- plus the resource-budget positive controls a validator that walks
// adversarial, untrusted JSON needs (022 SS5's own "a positive control proves the gate is real, not
// vacuous" discipline).

#include <cstdio>
#include <string>

#include "agentengine/core/json_schema_validator.hpp"
#include "agentengine/core/json_value.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

namespace json = agentengine::json;
namespace schema = agentengine::schema;

json::Value parse(std::string const& text, json::ParseBudget budget = {}) {
    auto v = json::parse(text, budget);
    if (!v) {
        std::fprintf(stderr, "FATAL: test fixture JSON failed to parse: %s\n", text.c_str());
        std::exit(1);
    }
    return *v;
}

}  // namespace

int main() {
    // --- V-1: a valid instance produces zero violations (the positive control every negative test
    // --- below needs, per 022 §5 -- proves "no violations" means genuinely valid, not "nothing ran") -
    {
        auto s = parse(R"({"type":"object","required":["name"],"properties":{"name":{"type":"string"},"age":{"type":"integer","minimum":0}}})");
        auto i = parse(R"({"name":"ada","age":36})");
        auto result = schema::validate_instance(s, i);
        check(result.has_value(), "V-1: validate_instance() itself succeeds");
        check(result.has_value() && result->empty(), "V-1: a genuinely valid instance produces zero violations");
    }

    // --- 006 §8 G2: a schema violation produces a real, structured, actionable diagnostic ----------
    {
        auto s = parse(R"({"type":"object","required":["name"],"properties":{"name":{"type":"string"}}})");
        auto i = parse(R"({"age":36})");
        auto result = schema::validate_instance(s, i);
        check(result.has_value(), "G2-1: validate_instance() succeeds (a violation is data, not a thrown error)");
        check(result.has_value() && result->size() == 1, "G2-1: exactly one violation -- missing required 'name'");
        if (result.has_value() && result->size() == 1) {
            check((*result)[0].message.find("name") != std::string::npos,
                  "G2-1: the violation message names the specific missing property, not a generic string");
        }
    }
    {
        auto s = parse(R"({"type":"string"})");
        auto i = parse(R"(42)");
        auto result = schema::validate_instance(s, i);
        check(result.has_value() && result->size() == 1 && (*result)[0].instance_path.empty() &&
                  (*result)[0].schema_path == "#/type",
              "G2-2: a top-level type mismatch is reported at the real schema_path ('#/type')");
    }
    {
        auto s = parse(R"({"type":"object","additionalProperties":false,"properties":{"a":{"type":"string"}}})");
        auto i = parse(R"({"a":"x","b":"unexpected"})");
        auto result = schema::validate_instance(s, i);
        check(result.has_value() && result->size() == 1 && (*result)[0].instance_path == "/b",
              "G2-3: additionalProperties:false rejects an undeclared property, at the real instance_path");
    }
    {
        auto s = parse(R"({"type":"array","items":{"type":"number"},"minItems":2})");
        auto i = parse(R"([1])");
        auto result = schema::validate_instance(s, i);
        check(result.has_value() && result->size() == 1,
              "G2-4: minItems violation reported (array too short)");
    }
    {
        auto s = parse(R"({"enum":["red","green","blue"]})");
        auto i = parse(R"("purple")");
        auto result = schema::validate_instance(s, i);
        check(result.has_value() && result->size() == 1, "G2-5: a value outside the declared enum is rejected");
    }
    {
        // A nested violation two levels down -- proves recursion through properties/items actually
        // reaches the real location, not just top-level checks.
        auto s = parse(
            R"({"type":"object","properties":{"items":{"type":"array","items":{"type":"object","required":["id"]}}}})");
        auto i = parse(R"({"items":[{"id":1},{"no_id":true}]})");
        auto result = schema::validate_instance(s, i);
        check(result.has_value() && result->size() == 1 && (*result)[0].instance_path == "/items/1",
              "G2-6: a violation nested two levels deep (object -> array -> object) is found at the "
              "correct instance_path");
    }

    // --- 015 §7 G2: a cyclic $ref is rejected with precise diagnostics -----------------------------
    {
        // Direct self-cycle: #/$defs/Node -> #/$defs/Node.
        auto s = parse(
            R"({"$defs":{"Node":{"type":"object","properties":{"next":{"$ref":"#/$defs/Node"}}}},"$ref":"#/$defs/Node"})");
        auto result = schema::validate_schema_well_formed(s);
        check(result.has_value(), "REF-1: validate_schema_well_formed() itself succeeds (a cycle is a "
                                   "violation, not a thrown error)");
        check(result.has_value() && !result->empty(), "REF-1: a self-referential $ref cycle is detected");
        if (result.has_value() && !result->empty()) {
            bool found_cycle_msg = false;
            for (auto const& v : *result) {
                if (v.message.find("cyclic") != std::string::npos) found_cycle_msg = true;
            }
            check(found_cycle_msg, "REF-1: the diagnostic explicitly says 'cyclic', not a generic failure");
        }
    }
    {
        // Indirect cycle: A -> B -> A.
        auto s = parse(
            R"({"$defs":{"A":{"$ref":"#/$defs/B"},"B":{"$ref":"#/$defs/A"}},"$ref":"#/$defs/A"})");
        auto result = schema::validate_schema_well_formed(s);
        check(result.has_value() && !result->empty(), "REF-2: an INDIRECT $ref cycle (A -> B -> A) is also detected");
    }
    {
        // Positive control: a DAG-shaped schema referencing the SAME $defs entry from two
        // independent, non-overlapping places is legitimate and must NOT be flagged as a cycle.
        auto s = parse(
            R"({"$defs":{"Leaf":{"type":"string"}},"type":"object","properties":{"a":{"$ref":"#/$defs/Leaf"},"b":{"$ref":"#/$defs/Leaf"}}})");
        auto result = schema::validate_schema_well_formed(s);
        check(result.has_value() && result->empty(),
              "REF-3 (positive control): the SAME $ref target reached from two independent, "
              "non-overlapping paths is NOT a cycle -- proves REF-1/REF-2 detect real cycles, not "
              "merely repeated visits");
    }
    {
        // A $ref to a target that doesn't exist -- a different, real diagnostic, not silently ignored.
        auto s = parse(R"({"$ref":"#/$defs/DoesNotExist"})");
        auto result = schema::validate_schema_well_formed(s);
        check(result.has_value() && !result->empty(), "REF-4: a $ref to a nonexistent target is flagged, not silently ignored");
    }

    // --- Resource budgets: adversarial input gets bounded work, never an unbounded hang/list --------
    {
        // A cyclic schema, VALIDATED AGAINST A FINITE INSTANCE -- proves validate_instance() itself
        // terminates against a cyclic schema (its own comment's claim: the instance's finite nesting
        // is what bounds it, not a separate cycle check) rather than hanging.
        auto s = parse(
            R"({"$defs":{"Node":{"type":"object","properties":{"next":{"$ref":"#/$defs/Node"},"val":{"type":"number"}}}},"$ref":"#/$defs/Node"})");
        auto i = parse(R"({"val":1,"next":{"val":2,"next":{"val":3}}})");
        auto result = schema::validate_instance(s, i);
        check(result.has_value() && result->empty(),
              "BUDGET-1: validate_instance() terminates and correctly validates a FINITE instance "
              "against a genuinely cyclic schema -- proves the 'instance bounds it, not a separate "
              "cycle check' design claim, not just asserted");
    }
    {
        // A genuinely deep instance exceeds max_depth and fails closed with a real diagnostic,
        // rather than a stack overflow or an unbounded recursion.
        std::string deep_schema = R"({"type":"object","properties":{"n":{"$ref":"#"}}})";
        std::string deep_instance = "{";
        for (int i = 0; i < 200; ++i) deep_instance += R"("n":{)";
        deep_instance += "}";
        for (int i = 0; i < 200; ++i) deep_instance += "}";
        auto s = parse(deep_schema);
        // This fixture is deliberately deeper than json::Parser's own default ParseBudget
        // (max_depth=64) -- it's exercising validate_instance()'s budget, not the parser's, so the
        // parse itself needs enough headroom to actually produce the 200-deep instance below.
        auto i = parse(deep_instance, json::ParseBudget{/*max_depth=*/256, /*max_nodes_visited=*/100'000});
        schema::ValidationBudget tight_depth;
        tight_depth.max_depth = 32;
        auto result = schema::validate_instance(s, i, tight_depth);
        check(result.has_value() && !result->empty(),
              "BUDGET-2: an instance deeper than max_depth fails closed with a real diagnostic, not a "
              "crash or an infinite recursion");
    }
    {
        // A huge, entirely VALID array must not walk unbounded work just because nothing is wrong --
        // the max_nodes_visited catch from self-red-team, proven directly, not just commented.
        std::string big_instance = "[";
        for (int i = 0; i < 5000; ++i) {
            if (i > 0) big_instance += ",";
            big_instance += "1";
        }
        big_instance += "]";
        auto s = parse(R"({"type":"array","items":{"type":"number"}})");
        auto i = parse(big_instance);
        schema::ValidationBudget tight_nodes;
        tight_nodes.max_nodes_visited = 100;
        auto result = schema::validate_instance(s, i, tight_nodes);
        check(result.has_value() && !result->empty(),
              "BUDGET-3: a huge but entirely VALID instance still fails closed once max_nodes_visited "
              "is exceeded -- total work is bounded independent of whether any real violation exists");
        // Positive control: the SAME instance, generously budgeted, validates cleanly -- proves
        // BUDGET-3's failure above is really the node budget acting, not the validator being broken.
        auto result_generous = schema::validate_instance(s, i);
        check(result_generous.has_value() && result_generous->empty(),
              "BUDGET-3 positive control: the identical instance, under the default generous budget, "
              "validates with zero violations");
    }
    {
        // A long (but acyclic) $ref chain exceeding max_ref_hops fails closed too -- not solely
        // reliant on cycle detection.
        std::string doc = R"({"$defs":{)";
        for (int i = 0; i < 50; ++i) {
            if (i > 0) doc += ",";
            doc += "\"S" + std::to_string(i) + "\":";
            doc += (i + 1 < 50) ? ("{\"$ref\":\"#/$defs/S" + std::to_string(i + 1) + "\"}")
                                 : "{\"type\":\"string\"}";
        }
        doc += R"(},"$ref":"#/$defs/S0"})";
        auto s = parse(doc);
        schema::ValidationBudget tight_hops;
        tight_hops.max_ref_hops = 10;
        auto result = schema::validate_schema_well_formed(s, tight_hops);
        check(result.has_value() && !result->empty(),
              "BUDGET-4: a long ACYCLIC $ref chain longer than max_ref_hops fails closed too -- "
              "the budget is a real backstop, not solely dependent on cycle detection");
    }

    std::printf(g_failures == 0 ? "test_json_schema_validator: OK\n" : "test_json_schema_validator: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
