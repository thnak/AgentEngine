#pragma once
// Implements 006-Tool-and-Function-Plane.md §8 G2 ("a negative suite including a schema violation
// produces a structured ToolResult{is_error}") and 015-Declarative-Agent-Format.md §7 G2 ("a
// negative corpus including cyclic $ref is rejected with precise diagnostics") -- gap-3 fix
// (2026-08-14, decisions/ADR-045-*.md). `core/json_schema.hpp`'s existing `AE_JSON_SCHEMA` machinery
// only ever generates a schema FROM a known C++ type, one direction; this file is the missing other
// direction, a real instance-against-a-schema-DOCUMENT check that works for any `json::Value` schema,
// not just ones this codebase generated itself.
//
// DELIBERATELY A SUBSET, not the full JSON Schema 2020-12 keyword vocabulary -- covers `type`,
// `enum`, `const`, `required`, `properties`, `additionalProperties` (boolean form only), `items`
// (single-schema form only, not tuple/`prefixItems`), `minItems`/`maxItems`, `minLength`/`maxLength`,
// `minimum`/`maximum`/`exclusiveMinimum`/`exclusiveMaximum`, and local `$ref`/`$defs` (a JSON Pointer
// into the SAME schema document -- resolving a `$ref` to an EXTERNAL file/URL is 015's own separate
// $ref-resolution step, gap #6, which must already have happened before a schema reaches this
// validator). NOT implemented, by deliberate design choice, not oversight: `pattern` (regex) --
// including it means either shipping a regex engine and bounding it against catastrophic backtracking
// (a real, hard problem this file structurally avoids by not having a regex engine at all, rather than
// trying to bound one) or `format`/`oneOf`/`anyOf`/`allOf`/`not` (schema boolean composition -- real,
// separate complexity, no gate in 006/015 currently requires it). A schema using an unsupported
// keyword is not silently ignored -- see `validate_schema_well_formed()`.
//
// Every entry point takes a `ValidationBudget` and fails closed (a structured, real violation or
// error) the moment ANY bound is exceeded, never silently truncates and reports "valid" -- an
// adversarial schema or instance (deep nesting, a huge property fan-out, a long or cyclic `$ref`
// chain) gets a bounded amount of work and a loud diagnostic, never an unbounded recursion or an
// unbounded violation list.

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::schema {

// ae-naming-lint: allow ValidationBudget — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ValidationBudget {
    std::uint32_t max_depth = 64;        // schema+instance co-descent bound (also the sole guard
                                          // against a runaway walk of validate_instance() -- see that
                                          // function's own comment for why this alone is sufficient
                                          // there, unlike the well-formedness checker below).
    std::uint32_t max_ref_hops = 32;      // longest allowed $ref resolution chain, cyclic or not.
    std::uint32_t max_violations = 100;   // collected violations cap -- an instance that violates a
                                          // schema thousands of times over (e.g. every element of a
                                          // huge array is the wrong type) still returns a bounded,
                                          // real, actionable list, not an unbounded one.
    // Self-red-team catch: max_violations alone does NOT bound total work for a huge but otherwise
    // VALID instance (a 10-million-element array with no violations never trips that budget, but
    // still walks 10 million nodes). This bounds total (schema-node, instance-node) visits directly,
    // independent of whether any violation was ever found.
    std::uint64_t max_nodes_visited = 100'000;
};

// ae-naming-lint: allow ValidationViolation — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct ValidationViolation {
    std::string instance_path;  // JSON-Pointer-shaped location in the INSTANCE, e.g. "/items/2/name"
    std::string schema_path;    // JSON-Pointer-shaped location in the SCHEMA that was violated
    std::string message;        // human-actionable, never a stack trace or internal identifier (026 §3)

    friend bool operator==(ValidationViolation const&, ValidationViolation const&) = default;
};

namespace detail {

// RFC 6901 JSON Pointer resolution, LOCAL ONLY (`root` is always the top of the SAME schema document
// -- a pointer beginning with anything other than "#" is a caller error, not a valid local $ref).
[[nodiscard]] inline json::Value const* resolve_json_pointer(json::Value const& root,
                                                                std::string_view pointer) {
    if (pointer.empty() || pointer == "#") return &root;
    if (pointer.size() < 2 || pointer[0] != '#' || pointer[1] != '/') return nullptr;
    json::Value const* cur = &root;
    std::string_view rest = pointer.substr(2);
    while (true) {
        auto slash = rest.find('/');
        std::string_view token = slash == std::string_view::npos ? rest : rest.substr(0, slash);
        std::string key;
        key.reserve(token.size());
        for (std::size_t i = 0; i < token.size(); ++i) {
            if (token[i] == '~' && i + 1 < token.size() && (token[i + 1] == '0' || token[i + 1] == '1')) {
                key += (token[i + 1] == '1') ? '/' : '~';
                ++i;
            } else {
                key += token[i];
            }
        }
        if (cur->is_object()) {
            cur = cur->find(key);
            if (cur == nullptr) return nullptr;
        } else if (cur->is_array()) {
            if (key.empty()) return nullptr;
            std::size_t idx = 0;
            for (char c : key) {
                if (c < '0' || c > '9') return nullptr;
                idx = idx * 10 + static_cast<std::size_t>(c - '0');
            }
            auto const& arr = cur->as_array();
            if (idx >= arr.size()) return nullptr;
            cur = &arr[idx];
        } else {
            return nullptr;
        }
        if (slash == std::string_view::npos) return cur;
        rest = rest.substr(slash + 1);
    }
}

[[nodiscard]] inline ValidationViolation make_violation(std::string instance_path,
                                                           std::string schema_path, std::string message) {
    return ValidationViolation{std::move(instance_path), std::move(schema_path), std::move(message)};
}

// `json::Value` deliberately has no `operator==` of its own (json_value.hpp) -- structural equality
// is exactly what `enum`/`const` need, nowhere else in this codebase, so it lives here rather than on
// the shared value type.
[[nodiscard]] inline bool json_value_equal(json::Value const& a, json::Value const& b) {
    if (a.kind() != b.kind()) return false;
    switch (a.kind()) {
        case json::value_kind::null: return true;
        case json::value_kind::boolean: return a.as_bool() == b.as_bool();
        case json::value_kind::number: return a.as_number() == b.as_number();
        case json::value_kind::string: return a.as_string() == b.as_string();
        case json::value_kind::array: {
            auto const& aa = a.as_array();
            auto const& bb = b.as_array();
            if (aa.size() != bb.size()) return false;
            for (std::size_t i = 0; i < aa.size(); ++i) {
                if (!json_value_equal(aa[i], bb[i])) return false;
            }
            return true;
        }
        case json::value_kind::object: {
            auto const& ao = a.as_object();
            auto const& bo = b.as_object();
            if (ao.size() != bo.size()) return false;
            for (auto const& [key, val] : ao) {
                json::Value const* other = b.find(key);
                if (other == nullptr || !json_value_equal(val, *other)) return false;
            }
            return true;
        }
    }
    return false;
}

}  // namespace detail

// Checks the SCHEMA DOCUMENT ITSELF for structural validity -- 015 §7 G2's own gate ("a negative
// corpus including cyclic $ref is rejected with precise diagnostics"). Unlike `validate_instance()`
// below, this has no instance to bound recursion against, so cycle detection here is load-bearing,
// not redundant: a real DFS over every `$ref` this schema (transitively) contains, tracking the set
// of pointers on the CURRENT resolution path (not a global visited set -- a DAG-shaped schema that
// references the same `$defs` entry from two independent, non-overlapping places is legitimate and
// must not be flagged; only a pointer already on the ACTIVE path is a real cycle).
[[nodiscard]] inline result<std::vector<ValidationViolation>> validate_schema_well_formed(
    json::Value const& schema, ValidationBudget budget = {}) {
    std::vector<ValidationViolation> violations;

    struct Walker {
        json::Value const& root;
        ValidationBudget const& budget;
        std::vector<ValidationViolation>& violations;
        std::vector<std::string> path_stack;  // $ref pointers currently being resolved, in order
        std::uint64_t nodes_visited = 0;

        [[nodiscard]] result<void> walk(json::Value const& node, std::string schema_path,
                                          std::uint32_t depth) {
            if (violations.size() >= budget.max_violations) return {};
            // Same self-red-team catch as validate_instance()'s own Walker: bounds total work
            // directly, independent of whether any violation is ever found (a schema with a huge
            // $defs/properties fan-out but no actual malformation must not walk it all regardless).
            if (++nodes_visited > budget.max_nodes_visited) {
                violations.push_back(detail::make_violation(
                    "", schema_path, "schema well-formedness check exceeded the configured "
                                     "max_nodes_visited budget"));
                return {};
            }
            if (depth > budget.max_depth) {
                violations.push_back(detail::make_violation(
                    "", schema_path, "schema nesting exceeds the configured max_depth budget"));
                return {};
            }
            if (!node.is_object()) return {};  // a bare boolean/other schema form -- nothing to walk

            if (json::Value const* ref = node.find("$ref"); ref != nullptr) {
                if (!ref->is_string()) {
                    violations.push_back(
                        detail::make_violation("", schema_path + "/$ref", "$ref must be a string"));
                    return {};
                }
                std::string const& pointer = ref->as_string();
                if (path_stack.size() >= budget.max_ref_hops) {
                    violations.push_back(detail::make_violation(
                        "", schema_path + "/$ref",
                        "$ref chain exceeds the configured max_ref_hops budget (pointer: " + pointer + ")"));
                    return {};
                }
                for (auto const& active : path_stack) {
                    if (active == pointer) {
                        std::string chain;
                        for (auto const& p : path_stack) chain += p + " -> ";
                        chain += pointer;
                        violations.push_back(detail::make_violation(
                            "", schema_path + "/$ref", "cyclic $ref detected: " + chain));
                        return {};
                    }
                }
                json::Value const* target = detail::resolve_json_pointer(root, pointer);
                if (target == nullptr) {
                    violations.push_back(detail::make_violation(
                        "", schema_path + "/$ref", "$ref does not resolve to anything in this document: " + pointer));
                    return {};
                }
                path_stack.push_back(pointer);
                auto r = walk(*target, schema_path + "/$ref", depth + 1);
                path_stack.pop_back();
                return r;
            }

            if (json::Value const* props = node.find("properties"); props != nullptr && props->is_object()) {
                for (auto const& [key, sub] : props->as_object()) {
                    if (auto r = walk(sub, schema_path + "/properties/" + key, depth + 1); !r) return r;
                }
            }
            if (json::Value const* items = node.find("items"); items != nullptr) {
                if (auto r = walk(*items, schema_path + "/items", depth + 1); !r) return r;
            }
            if (json::Value const* defs = node.find("$defs"); defs != nullptr && defs->is_object()) {
                for (auto const& [key, sub] : defs->as_object()) {
                    if (auto r = walk(sub, schema_path + "/$defs/" + key, depth + 1); !r) return r;
                }
            }
            return {};
        }
    };

    Walker w{schema, budget, violations, {}};
    if (auto r = w.walk(schema, "#", 0); !r) return std::unexpected(r.error());
    return violations;
}

// Validates `instance` against `schema`, collecting structured violations (006 §8 G2) -- empty
// return means valid. Bounded solely by `max_depth` co-descent through schema AND instance together
// (not a separate cycle check): `$ref` is only ever resolved when also stepping into a new INSTANCE
// node (an object property, an array element), so a finite instance structurally cannot drive this
// into an infinite loop even against a cyclic schema -- the instance's own finite nesting is what
// stops it. `validate_schema_well_formed()` above is the right tool for checking a schema is sound
// independent of any instance; this function does not re-derive that check, callers wanting both
// call both.
[[nodiscard]] inline result<std::vector<ValidationViolation>> validate_instance(
    json::Value const& schema, json::Value const& instance, ValidationBudget budget = {}) {
    std::vector<ValidationViolation> violations;

    struct Walker {
        json::Value const& root;
        ValidationBudget const& budget;
        std::vector<ValidationViolation>& violations;
        std::uint32_t ref_hops = 0;
        std::uint64_t nodes_visited = 0;
        bool node_budget_hit = false;  // latched once, so the resulting violation is added exactly
                                        // once rather than once per remaining loop iteration.

        [[nodiscard]] bool budget_exceeded() const {
            return violations.size() >= budget.max_violations || node_budget_hit;
        }

        void add(std::string instance_path, std::string schema_path, std::string message) {
            if (!budget_exceeded()) {
                violations.push_back(
                    detail::make_violation(std::move(instance_path), std::move(schema_path), std::move(message)));
            }
        }

        void walk(json::Value const& schema_node, json::Value const& inst, std::string instance_path,
                  std::string schema_path, std::uint32_t depth) {
            if (budget_exceeded()) return;
            // Self-red-team catch: bounds TOTAL work directly, independent of whether any violation
            // is ever found -- a huge but entirely VALID instance (e.g. a 10-million-element array)
            // must not walk all 10 million nodes just because nothing was wrong with any of them.
            if (++nodes_visited > budget.max_nodes_visited) {
                // Found by the test that proves this exact path (BUDGET-3): `add()` itself checks
                // `budget_exceeded()`, which now includes `node_budget_hit` -- setting that flag
                // BEFORE calling add() would silently block the very violation that reports the
                // exceeded budget. Must add first, latch second.
                add(instance_path, schema_path,
                    "validation exceeded the configured max_nodes_visited budget");
                node_budget_hit = true;
                return;
            }
            if (depth > budget.max_depth) {
                add(instance_path, schema_path, "instance nesting exceeds the configured max_depth budget");
                return;
            }
            if (!schema_node.is_object()) return;  // an empty/boolean-true schema accepts anything

            if (json::Value const* ref = schema_node.find("$ref"); ref != nullptr && ref->is_string()) {
                if (ref_hops >= budget.max_ref_hops) {
                    add(instance_path, schema_path + "/$ref",
                        "$ref chain exceeds the configured max_ref_hops budget");
                    return;
                }
                json::Value const* target = detail::resolve_json_pointer(root, ref->as_string());
                if (target == nullptr) {
                    add(instance_path, schema_path + "/$ref",
                        "$ref does not resolve to anything in this document: " + ref->as_string());
                    return;
                }
                ++ref_hops;
                walk(*target, inst, instance_path, schema_path + "/$ref", depth + 1);
                --ref_hops;
                return;
            }

            if (json::Value const* type = schema_node.find("type"); type != nullptr && type->is_string()) {
                std::string const& t = type->as_string();
                // "integer" is checked as "a finite number equal to its own floor" -- deliberately
                // NOT a round-trip through std::int64_t (a self-red-team catch: casting an
                // out-of-int64_t-range double is undefined behavior in C++, and nothing here bounds
                // an adversarial instance's numbers to that range beforehand).
                bool ok = (t == "string" && inst.is_string()) || (t == "boolean" && inst.is_bool()) ||
                          (t == "null" && inst.is_null()) || (t == "array" && inst.is_array()) ||
                          (t == "object" && inst.is_object()) ||
                          (t == "number" && inst.is_number()) ||
                          (t == "integer" && inst.is_number() && std::isfinite(inst.as_number()) &&
                           inst.as_number() == std::floor(inst.as_number()));
                if (!ok) {
                    add(instance_path, schema_path + "/type", "expected type '" + t + "'");
                    return;  // further keyword checks below assume the right shape; stop here, matching
                             // 006 §3 step 2's "reject, do not coerce" -- a type mismatch is the whole
                             // violation, not a springboard for a cascade of misleading follow-on ones.
                }
            }

            if (json::Value const* enum_v = schema_node.find("enum"); enum_v != nullptr && enum_v->is_array()) {
                bool matched = false;
                for (auto const& allowed : enum_v->as_array()) {
                    if (detail::json_value_equal(allowed, inst)) { matched = true; break; }
                }
                if (!matched) add(instance_path, schema_path + "/enum", "value is not one of the allowed enum values");
            }
            if (json::Value const* const_v = schema_node.find("const"); const_v != nullptr) {
                if (!detail::json_value_equal(*const_v, inst)) {
                    add(instance_path, schema_path + "/const", "value does not equal the required const value");
                }
            }

            if (inst.is_object()) {
                if (json::Value const* required = schema_node.find("required");
                    required != nullptr && required->is_array()) {
                    for (auto const& name_v : required->as_array()) {
                        if (!name_v.is_string()) continue;
                        if (inst.find(name_v.as_string()) == nullptr) {
                            add(instance_path, schema_path + "/required",
                                "missing required property '" + name_v.as_string() + "'");
                        }
                    }
                }
                json::Value const* props = schema_node.find("properties");
                if (json::Value const* additional = schema_node.find("additionalProperties");
                    additional != nullptr && additional->is_bool() && !additional->as_bool()) {
                    for (auto const& [key, _] : inst.as_object()) {
                        if (props == nullptr || props->find(key) == nullptr) {
                            add(instance_path + "/" + key, schema_path + "/additionalProperties",
                                "property '" + key + "' is not declared and additionalProperties is false");
                        }
                    }
                }
                if (props != nullptr && props->is_object()) {
                    for (auto const& [key, sub_schema] : props->as_object()) {
                        if (json::Value const* v = inst.find(key); v != nullptr) {
                            walk(sub_schema, *v, instance_path + "/" + key, schema_path + "/properties/" + key,
                                 depth + 1);
                            if (budget_exceeded()) return;
                        }
                    }
                }
            }

            if (inst.is_array()) {
                auto const& arr = inst.as_array();
                if (json::Value const* min_items = schema_node.find("minItems");
                    min_items != nullptr && min_items->is_number() && arr.size() < min_items->as_number()) {
                    add(instance_path, schema_path + "/minItems", "array has fewer than the required minimum items");
                }
                if (json::Value const* max_items = schema_node.find("maxItems");
                    max_items != nullptr && max_items->is_number() && arr.size() > max_items->as_number()) {
                    add(instance_path, schema_path + "/maxItems", "array has more than the allowed maximum items");
                }
                if (json::Value const* items = schema_node.find("items"); items != nullptr) {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        walk(*items, arr[i], instance_path + "/" + std::to_string(i), schema_path + "/items",
                             depth + 1);
                        if (budget_exceeded()) return;
                    }
                }
            }

            if (inst.is_string()) {
                std::size_t const len = inst.as_string().size();
                if (json::Value const* min_len = schema_node.find("minLength");
                    min_len != nullptr && min_len->is_number() && len < min_len->as_number()) {
                    add(instance_path, schema_path + "/minLength", "string is shorter than the required minLength");
                }
                if (json::Value const* max_len = schema_node.find("maxLength");
                    max_len != nullptr && max_len->is_number() && len > max_len->as_number()) {
                    add(instance_path, schema_path + "/maxLength", "string is longer than the allowed maxLength");
                }
            }

            if (inst.is_number()) {
                double const n = inst.as_number();
                if (json::Value const* min_v = schema_node.find("minimum");
                    min_v != nullptr && min_v->is_number() && n < min_v->as_number()) {
                    add(instance_path, schema_path + "/minimum", "value is less than the required minimum");
                }
                if (json::Value const* max_v = schema_node.find("maximum");
                    max_v != nullptr && max_v->is_number() && n > max_v->as_number()) {
                    add(instance_path, schema_path + "/maximum", "value is greater than the allowed maximum");
                }
                if (json::Value const* emin = schema_node.find("exclusiveMinimum");
                    emin != nullptr && emin->is_number() && n <= emin->as_number()) {
                    add(instance_path, schema_path + "/exclusiveMinimum",
                        "value is not strictly greater than the required exclusiveMinimum");
                }
                if (json::Value const* emax = schema_node.find("exclusiveMaximum");
                    emax != nullptr && emax->is_number() && n >= emax->as_number()) {
                    add(instance_path, schema_path + "/exclusiveMaximum",
                        "value is not strictly less than the required exclusiveMaximum");
                }
            }
        }
    };

    Walker w{schema, budget, violations};
    w.walk(schema, instance, "", "#", 0);
    return violations;
}

}  // namespace agentengine::schema
