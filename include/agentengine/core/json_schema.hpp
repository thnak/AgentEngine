#pragma once
// Implements 006-Tool-and-Function-Plane.md §1 — "Schemas are derived from the argument/result
// types at compile time (Quark 016's one-describe discipline), emitted as JSON Schema 2020-12."
//
// C++23 has no compile-time reflection (P2996 lands in C++26), so field *names* cannot be pulled
// out of a struct definition by the compiler alone -- the RFC's own `struct Args { std::string
// query; int max_results = 5; };` example (no macro shown) is therefore aspirational until C++26.
// Until then, every schema-bearing type pairs its definition with AE_JSON_SCHEMA(Type, members...),
// the same one-description-drives-everything shape Quark's QUARK_SERIALIZE already established for
// wire encoding (016) -- here the "everything" is one thing (a JSON Schema string) instead of
// three, but the discipline (one field list, found by ADL, never hand-duplicated) is the same one
// 006 §1 cites.
//
// Field types are read via decltype(declval<Type&>().member) -- no instance is constructed, so
// Args/Reply types need no default constructor.  "required" is derived structurally: a
// std::optional<T> member is not required, everything else is -- the RFC's own `int max_results =
// 5` example (a non-optional field with a default) is NOT detected as optional under this rule
// (default-member-initializer detection also needs reflection); a tool author who wants a field
// absent from `required` wraps it in std::optional.
//
// AE_JSON_SCHEMA(Type, member...) generates three things from the one field list, not just the
// schema string: `ae_to_json`/`ae_from_json` (this header) round-trip a real instance of Type
// through core/json_value.hpp's dependency-free Value type -- the actual (de)serialization the
// tool pipeline (006 §3 steps 2/9, core/tool_pipeline.hpp) needs at runtime, since the schema
// string alone only describes shape, it doesn't parse anything. `ae_from_json` needs Type to be
// default-constructible (unlike schema/to_json, which work from types alone) because it must
// materialize a real instance to assign fields into.

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "quark/core/describe.hpp"  // reused only for its QUARK_FOR_EACH variadic-expansion macro

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"

namespace agentengine::schema {

// Tag-dispatch target for ADL lookup of a type's AE_JSON_SCHEMA-generated description -- avoids
// requiring an instance of Type (some Args/Reply types may not be default-constructible).
template <class T>
struct type_tag {};

// Accumulates one object schema's "properties"/"required" as it's built field by field.
class ObjectBuilder {
public:
    void add_field(std::string_view name, std::string const& type_fragment, bool required) {
        if (!properties_.empty()) properties_ += ',';
        properties_ += '"';
        properties_ += name;
        properties_ += "\":";
        properties_ += type_fragment;
        if (required) {
            if (!required_.empty()) required_ += ',';
            required_ += '"';
            required_ += name;
            required_ += '"';
        }
    }

    [[nodiscard]] std::string build() const {
        std::string out = "{\"type\":\"object\",\"properties\":{";
        out += properties_;
        out += '}';
        if (!required_.empty()) {
            out += ",\"required\":[";
            out += required_;
            out += ']';
        }
        out += '}';
        return out;
    }

private:
    std::string properties_;
    std::string required_;
};

namespace detail {

template <class T>
struct is_optional : std::false_type {};
template <class U>
struct is_optional<std::optional<U>> : std::true_type {};
template <class T>
inline constexpr bool is_optional_v = is_optional<std::remove_cvref_t<T>>::value;

template <class T>
struct is_vector : std::false_type {};
template <class U, class A>
struct is_vector<std::vector<U, A>> : std::true_type {};
template <class T>
inline constexpr bool is_vector_v = is_vector<std::remove_cvref_t<T>>::value;

template <class T>
struct optional_inner {
    using type = T;
};
template <class U>
struct optional_inner<std::optional<U>> {
    using type = U;
};

template <class T>
struct vector_inner {
    using type = T;
};
template <class U, class A>
struct vector_inner<std::vector<U, A>> {
    using type = U;
};

// A type has its own AE_JSON_SCHEMA description iff ae_json_schema(type_tag<T>{}) resolves by ADL.
template <class T>
concept HasJsonSchema = requires(type_tag<T> tag) {
    { ae_json_schema(tag) } -> std::convertible_to<std::string>;
};

}  // namespace detail

template <class T>
std::string type_fragment();

template <class T>
[[nodiscard]] constexpr bool is_required() noexcept {
    return !detail::is_optional_v<T>;
}

template <class T>
std::string type_fragment() {
    using U = std::remove_cvref_t<T>;
    if constexpr (detail::is_optional_v<U>) {
        return type_fragment<typename detail::optional_inner<U>::type>();
    } else if constexpr (std::is_same_v<U, bool>) {
        return "{\"type\":\"boolean\"}";
    } else if constexpr (std::is_same_v<U, std::string>) {
        return "{\"type\":\"string\"}";
    } else if constexpr (std::is_enum_v<U> || (std::is_integral_v<U> && !std::is_same_v<U, bool>)) {
        return "{\"type\":\"integer\"}";
    } else if constexpr (std::is_floating_point_v<U>) {
        return "{\"type\":\"number\"}";
    } else if constexpr (detail::is_vector_v<U>) {
        return "{\"type\":\"array\",\"items\":" +
               type_fragment<typename detail::vector_inner<U>::type>() + "}";
    } else if constexpr (detail::HasJsonSchema<U>) {
        return ae_json_schema(type_tag<U>{});
    } else {
        static_assert(detail::HasJsonSchema<U>,
            "no JSON Schema mapping for this field type -- use a bool/integral/floating-point/"
            "std::string/std::optional<T>/std::vector<T>, or give the nested type its own "
            "AE_JSON_SCHEMA(...) description");
        return {};
    }
}

// The full schema for a type carrying its own AE_JSON_SCHEMA(...) description (an Args or Reply).
template <detail::HasJsonSchema T>
std::string json_schema_of() {
    return ae_json_schema(type_tag<T>{});
}

// -- Serialization: T -> json::Value ----------------------------------------------------------

template <class T>
json::Value to_json_value(T const& v);

// Accumulates one object's members for ae_to_json; a std::optional<T> field with no value is
// omitted from the object entirely (never emitted as JSON null) -- symmetric with from_json_field
// below, where an absent key on an optional field also means "no value", not "value is null".
class ToJsonBuilder {
public:
    template <class T>
    void add_field(std::string_view name, T const& value) {
        using U = std::remove_cvref_t<T>;
        if constexpr (detail::is_optional_v<U>) {
            if (value.has_value()) members_.emplace_back(std::string(name), to_json_value(*value));
        } else {
            members_.emplace_back(std::string(name), to_json_value(value));
        }
    }

    [[nodiscard]] json::Value build() const { return json::Value::make_object(members_); }

private:
    std::vector<std::pair<std::string, json::Value>> members_;
};

template <class T>
json::Value to_json_value(T const& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        return json::Value::make_bool(v);
    } else if constexpr (std::is_same_v<U, std::string>) {
        return json::Value::make_string(v);
    } else if constexpr (std::is_enum_v<U>) {
        return json::Value::make_number(
            static_cast<double>(static_cast<std::underlying_type_t<U>>(v)));
    } else if constexpr (std::is_integral_v<U> || std::is_floating_point_v<U>) {
        return json::Value::make_number(static_cast<double>(v));
    } else if constexpr (detail::is_vector_v<U>) {
        std::vector<json::Value> items;
        items.reserve(v.size());
        for (auto const& item : v) items.push_back(to_json_value(item));
        return json::Value::make_array(std::move(items));
    } else if constexpr (detail::HasJsonSchema<U>) {
        return ae_to_json(v);  // ADL to the nested type's own AE_JSON_SCHEMA-generated codec
    } else {
        static_assert(detail::HasJsonSchema<U>,
            "no JSON serialization for this field type -- use a bool/integral/floating-point/"
            "std::string/std::optional<T>/std::vector<T>, or give the nested type its own "
            "AE_JSON_SCHEMA(...) description");
        return json::Value::make_null();
    }
}

template <detail::HasJsonSchema T>
json::Value to_json(T const& v) {
    return ae_to_json(v);
}

// -- Deserialization: json::Value -> T, reject-not-coerce (006 §3 step 2) ----------------------

template <class T>
result<T> from_json_value(json::Value const& v, std::string_view field_name);

namespace detail {
inline error type_mismatch(std::string_view field_name, std::string_view expected) {
    return error{failure_class::contract,
                 "field '" + std::string(field_name) + "' must be " + std::string(expected),
                 "json.field_type_mismatch"};
}
}  // namespace detail

// Looks up `field_name` in `object` and decodes it as FieldType. A missing key is fine (and
// resolves to an empty optional) iff FieldType is std::optional<U>; otherwise it is a contract
// error -- this is the "required" half of the same rule ObjectBuilder's schema output states.
template <class FieldType>
result<FieldType> from_json_field(json::Value const& object, std::string_view field_name) {
    json::Value const* found = object.find(field_name);
    if (!found) {
        if constexpr (detail::is_optional_v<FieldType>) {
            return FieldType{};
        } else {
            return std::unexpected(error{failure_class::contract,
                "missing required field '" + std::string(field_name) + "'",
                "json.missing_required_field"});
        }
    }
    return from_json_value<FieldType>(*found, field_name);
}

template <class T>
result<T> from_json_value(json::Value const& v, std::string_view field_name) {
    using U = std::remove_cvref_t<T>;
    if constexpr (detail::is_optional_v<U>) {
        using Inner = typename detail::optional_inner<U>::type;
        if (v.is_null()) return U{std::nullopt};
        auto inner = from_json_value<Inner>(v, field_name);
        if (!inner) return std::unexpected(inner.error());
        return U{std::move(*inner)};
    } else if constexpr (std::is_same_v<U, bool>) {
        if (!v.is_bool()) return std::unexpected(detail::type_mismatch(field_name, "a boolean"));
        return v.as_bool();
    } else if constexpr (std::is_same_v<U, std::string>) {
        if (!v.is_string()) return std::unexpected(detail::type_mismatch(field_name, "a string"));
        return v.as_string();
    } else if constexpr (std::is_enum_v<U>) {
        if (!v.is_number()) return std::unexpected(detail::type_mismatch(field_name, "an integer"));
        return static_cast<U>(static_cast<std::underlying_type_t<U>>(v.as_number()));
    } else if constexpr (std::is_integral_v<U>) {
        if (!v.is_number()) return std::unexpected(detail::type_mismatch(field_name, "an integer"));
        return static_cast<U>(v.as_number());
    } else if constexpr (std::is_floating_point_v<U>) {
        if (!v.is_number()) return std::unexpected(detail::type_mismatch(field_name, "a number"));
        return static_cast<U>(v.as_number());
    } else if constexpr (detail::is_vector_v<U>) {
        using Inner = typename detail::vector_inner<U>::type;
        if (!v.is_array()) return std::unexpected(detail::type_mismatch(field_name, "an array"));
        U out;
        for (auto const& item : v.as_array()) {
            auto elem = from_json_value<Inner>(item, field_name);
            if (!elem) return std::unexpected(elem.error());
            out.push_back(std::move(*elem));
        }
        return out;
    } else if constexpr (detail::HasJsonSchema<U>) {
        if (!v.is_object()) return std::unexpected(detail::type_mismatch(field_name, "an object"));
        return ae_from_json(v, type_tag<U>{});  // ADL to the nested type's own generated codec
    } else {
        static_assert(detail::HasJsonSchema<U>,
            "no JSON deserialization for this field type -- use a bool/integral/floating-point/"
            "std::string/std::optional<T>/std::vector<T>, or give the nested type its own "
            "AE_JSON_SCHEMA(...) description");
        return std::unexpected(error{failure_class::contract, "unsupported field type", "json.unsupported_type"});
    }
}

template <detail::HasJsonSchema T>
result<T> from_json(json::Value const& v) {
    return ae_from_json(v, type_tag<T>{});
}

}  // namespace agentengine::schema

// AE_JSON_SCHEMA(Type, member1, member2, ...) -- up to 16 members (QUARK_FOR_EACH's own limit;
// quark/core/describe.hpp is reused here rather than re-deriving the same variadic-counting
// preprocessor machinery, per 006 §1's explicit "Quark 016's one-describe discipline" citation).
// Expands to a `ae_json_schema(type_tag<Type>)` free function found by ADL from type_fragment<T>().
//
// AE_JSON_SCHEMA_FIELD_ cannot reference `Type` directly: it is a macro parameter of the
// *enclosing* AE_JSON_SCHEMA invocation, not of AE_JSON_SCHEMA_FIELD_ itself, and macro expansion
// is plain text substitution with no shared parameter scope across two different macros. Instead,
// AE_JSON_SCHEMA introduces a local `using AeJsonSchemaSelf = Type;` alias before expanding the
// per-field macro; `AeJsonSchemaSelf` is then resolved by ordinary C++ name lookup at compile time
// (not by the preprocessor), which does see the alias declared earlier in the same function body.
#define AE_JSON_SCHEMA_FIELD_(member)                                                        \
    ob.add_field(                                                                            \
        #member,                                                                             \
        ::agentengine::schema::type_fragment<decltype(std::declval<AeJsonSchemaSelf&>().member)>(), \
        ::agentengine::schema::is_required<decltype(std::declval<AeJsonSchemaSelf&>().member)>());

#define AE_JSON_TO_FIELD_(member) ae_tb.add_field(#member, self.member);

#define AE_JSON_FROM_FIELD_(member)                                                          \
    {                                                                                        \
        auto ae_field_result = ::agentengine::schema::from_json_field<                       \
            decltype(std::declval<AeJsonSchemaSelf&>().member)>(value, #member);             \
        if (!ae_field_result) return std::unexpected(ae_field_result.error());               \
        out.member = std::move(*ae_field_result);                                            \
    }

#define AE_JSON_SCHEMA(Type, ...)                                                            \
    inline std::string ae_json_schema(::agentengine::schema::type_tag<Type>) {               \
        using AeJsonSchemaSelf = Type;                                                       \
        ::agentengine::schema::ObjectBuilder ob;                                             \
        QUARK_FOR_EACH(AE_JSON_SCHEMA_FIELD_, __VA_ARGS__)                                   \
        return ob.build();                                                                   \
    }                                                                                        \
    inline ::agentengine::json::Value ae_to_json(Type const& self) {                         \
        using AeJsonSchemaSelf = Type;                                                       \
        ::agentengine::schema::ToJsonBuilder ae_tb;                                          \
        QUARK_FOR_EACH(AE_JSON_TO_FIELD_, __VA_ARGS__)                                       \
        return ae_tb.build();                                                                \
    }                                                                                        \
    inline ::agentengine::result<Type> ae_from_json(::agentengine::json::Value const& value,  \
                                                      ::agentengine::schema::type_tag<Type>) { \
        using AeJsonSchemaSelf = Type;                                                       \
        if (!value.is_object()) {                                                            \
            return std::unexpected(::agentengine::error{::agentengine::failure_class::contract, \
                "expected a JSON object for " #Type, "json.expected_object"});               \
        }                                                                                     \
        Type out{};                                                                          \
        QUARK_FOR_EACH(AE_JSON_FROM_FIELD_, __VA_ARGS__)                                     \
        return out;                                                                          \
    }
