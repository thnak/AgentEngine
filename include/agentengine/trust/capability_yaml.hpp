#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §5 step 5 (the YAML `capabilities:` block) and
// rfcs/015-Declarative-Agent-Format.md §2 / I6: a declarative agent's capability CEILING, parsed into the same
// `cap::` values the C++ form (`Capabilities<cap::decl::...>`) produces, so a `per_request` YAML agent can declare
// the most authority it will accept (ADR-244 §2.3, red-team D14) instead of being refused for having none.
//
//   capabilities:
//     fs_read:
//       - { mount: work, path_prefix: /src, size_cap_bytes: 1048576 }
//     fs_write:
//       - { mount: work, path_prefix: /out, quota_bytes: 10485760, file_count_cap: unbounded }
//     net_out:
//       - { hosts: ["api.example.com:443:https"], byte_cap: 1048576, methods: [GET] }   # methods: any | [..]
//     native_exec:
//       - { program: node, worktree_mount: work, cpu_ms_cap: 30000, wall_ms_cap: 30000, memory_bytes_cap: unbounded }
//
// Every kind holds a sequence of entries (a document can grant more than one mount or host set). Nothing is
// defaulted: each limit is a positive integer or the word `unbounded` (trust/grant_limit.hpp), a path prefix is a
// stated non-empty path ("/" is the whole mount), a host list is non-empty, and "any HTTP method" is the word `any`,
// never an empty list. An unknown kind or key, a wrong type, or a missing field is refused with a named error and
// nothing is skipped, so a misspelt `fs_wirte:` cannot silently mean "no capability" and a typo'd `byte_cap` cannot
// silently mean "uncapped". Only the kinds that carry a limit are spelled in a document; the rest (clock, entropy,
// secrets, agent calls) are refused with the list of supported kinds until each has its own reviewed spelling.
//
// Host-authored configuration only (I3): nothing model-derived reaches this parser, and an overlay may not widen the
// block (015 §5), which is the loader's rule, not this parser's.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/grant_limit.hpp"
#include "agentengine/trust/native_exec_grant.hpp"

namespace agentengine::trust {

namespace capability_yaml_detail {

[[nodiscard]] inline agentengine::error invalid(std::string where, std::string why) {
    return agentengine::error{agentengine::failure_class::contract,
                              "capabilities." + std::move(where) + ": " + std::move(why), "capabilities.invalid"};
}

// Refuses a key the kind does not define; returns the offending key or "".
[[nodiscard]] inline std::string unknown_key(json::Value const& entry, std::initializer_list<std::string_view> known) {
    for (auto const& [key, value] : entry.as_object()) {
        (void)value;
        bool ok = false;
        for (auto k : known) ok = ok || key == k;
        if (!ok) return key;
    }
    return {};
}

[[nodiscard]] inline agentengine::result<std::string> required_string(json::Value const& entry, std::string_view key,
                                                                      std::string const& where) {
    auto const* v = entry.find(std::string(key));
    if (v == nullptr || !v->is_string() || v->as_string().empty()) {
        return std::unexpected(invalid(where, "'" + std::string(key) + "' is required and must be a non-empty string"));
    }
    return v->as_string();
}

[[nodiscard]] inline agentengine::result<cap::FsRead> parse_fs_read(json::Value const& e, std::string const& where) {
    if (auto k = unknown_key(e, {"mount", "path_prefix", "size_cap_bytes"}); !k.empty()) {
        return std::unexpected(invalid(where, "unknown key '" + k + "'"));
    }
    auto mount = required_string(e, "mount", where);
    if (!mount) return std::unexpected(mount.error());
    auto prefix = required_string(e, "path_prefix", where);
    if (!prefix) return std::unexpected(prefix.error());
    auto size = parse_grant_limit(e, "size_cap_bytes");
    if (!size) return std::unexpected(size.error());
    return cap::FsRead{std::move(*mount), std::move(*prefix), *size};
}

[[nodiscard]] inline agentengine::result<cap::FsWrite> parse_fs_write(json::Value const& e, std::string const& where) {
    if (auto k = unknown_key(e, {"mount", "path_prefix", "quota_bytes", "file_count_cap"}); !k.empty()) {
        return std::unexpected(invalid(where, "unknown key '" + k + "'"));
    }
    auto mount = required_string(e, "mount", where);
    if (!mount) return std::unexpected(mount.error());
    auto prefix = required_string(e, "path_prefix", where);
    if (!prefix) return std::unexpected(prefix.error());
    auto quota = parse_grant_limit(e, "quota_bytes");
    if (!quota) return std::unexpected(quota.error());
    auto files = parse_grant_limit(e, "file_count_cap");
    if (!files) return std::unexpected(files.error());
    return cap::FsWrite{std::move(*mount), std::move(*prefix), *quota, *files};
}

[[nodiscard]] inline agentengine::result<cap::NetOut> parse_net_out(json::Value const& e, std::string const& where) {
    if (auto k = unknown_key(e, {"hosts", "byte_cap", "methods"}); !k.empty()) {
        return std::unexpected(invalid(where, "unknown key '" + k + "'"));
    }
    cap::NetOut out{{}, cap::Limit::unbounded(), {}};
    auto const* hosts = e.find("hosts");
    if (hosts == nullptr || !hosts->is_array() || hosts->as_array().empty()) {
        return std::unexpected(invalid(where, "'hosts' is required and must be a non-empty list"));
    }
    for (auto const& h : hosts->as_array()) {
        if (!h.is_string() || h.as_string().empty()) {
            return std::unexpected(invalid(where, "every 'hosts' entry must be a non-empty string"));
        }
        out.host_allowlist.push_back(h.as_string());
    }
    auto bytes = parse_grant_limit(e, "byte_cap");
    if (!bytes) return std::unexpected(bytes.error());
    out.byte_cap = *bytes;
    auto const* methods = e.find("methods");
    if (methods == nullptr) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "capabilities." + where + ": 'methods' is required: a list of HTTP methods, or the word 'any'",
            "choice.missing.grant.methods"});
    }
    if (methods->is_string()) {
        if (methods->as_string() != "any") {
            return std::unexpected(invalid(where, "'methods' must be a list of methods, or the word 'any'"));
        }
        // Spelled `any`: an empty restriction list is how the capability stores "no method restriction".
    } else if (methods->is_array() && !methods->as_array().empty()) {
        for (auto const& m : methods->as_array()) {
            if (!m.is_string() || m.as_string().empty()) {
                return std::unexpected(invalid(where, "every 'methods' entry must be a non-empty string"));
            }
            out.method_restrictions.push_back(m.as_string());
        }
    } else {
        return std::unexpected(invalid(where, "'methods' must be a non-empty list of methods, or the word 'any'"));
    }
    return out;
}

}  // namespace capability_yaml_detail

// `capabilities` is the value of a document's `spec.capabilities` key.
[[nodiscard]] inline agentengine::result<std::vector<Capability>> parse_capability_ceiling(json::Value const& caps) {
    namespace d = capability_yaml_detail;
    if (!caps.is_object()) {
        return std::unexpected(d::invalid("", "must be a mapping of capability kind to a list of entries"));
    }
    std::vector<Capability> out;
    for (auto const& [kind, entries] : caps.as_object()) {
        if (kind != "fs_read" && kind != "fs_write" && kind != "net_out" && kind != "native_exec") {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "capabilities." + kind + ": not a capability a document can declare; choose from: fs_read, fs_write, "
                "net_out, native_exec",
                "capabilities.unsupported_kind"});
        }
        if (!entries.is_array() || entries.as_array().empty()) {
            return std::unexpected(d::invalid(kind, "must be a non-empty list of entries"));
        }
        std::size_t index = 0;
        for (auto const& entry : entries.as_array()) {
            std::string const where = kind + "[" + std::to_string(index++) + "]";
            if (!entry.is_object()) return std::unexpected(d::invalid(where, "must be a mapping"));
            if (kind == "fs_read") {
                auto g = d::parse_fs_read(entry, where);
                if (!g) return std::unexpected(g.error());
                out.push_back(Capability{std::move(*g)});
            } else if (kind == "fs_write") {
                auto g = d::parse_fs_write(entry, where);
                if (!g) return std::unexpected(g.error());
                out.push_back(Capability{std::move(*g)});
            } else if (kind == "net_out") {
                auto g = d::parse_net_out(entry, where);
                if (!g) return std::unexpected(g.error());
                out.push_back(Capability{std::move(*g)});
            } else {
                auto g = parse_native_exec_grant(entry);
                if (!g) return std::unexpected(g.error());
                out.push_back(Capability{std::move(*g)});
            }
        }
    }
    return out;
}

}  // namespace agentengine::trust
