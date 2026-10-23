// Proves decisions/ADR-244-required-choices-step-1b.md §5 step 5 (the YAML `capabilities:` block): a document's
// capability ceiling parses into the same `cap::` values the C++ form produces (I6), requires every limit to be
// stated, and refuses anything it does not understand rather than skipping it.
//
//   Y1  a full document parses to the values the C++ form states (field by field), one per entry.
//   Y2  the parsed ceiling and the equivalent C++ ceiling subsume each other (the attenuation rules cannot tell them
//       apart), and a larger request than the document stated is refused -- the ceiling really bounds.
//   Y3  each missing limit is `choice.missing.grant.<key>`; `0` is `choice.zero_bound`; `unbounded` is accepted.
//   Y4  an unknown kind, an unknown key, a wrong type, an empty list, an empty path prefix and a missing `methods`
//       are each refused with a named error.
//   Y5  positive control: the strictness corpus is not refusing everything -- the minimal valid document parses.

#include <iostream>
#include <string>
#include <string_view>

#include "agentengine/core/json_value.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/capability_yaml.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// The parse result of a JSON text standing in for a document's `spec.capabilities` value.
agentengine::result<std::vector<Capability>> parse_caps(std::string_view json_text) {
    auto v = json::parse(json_text);
    if (!v) return std::unexpected(v.error());
    return trust::parse_capability_ceiling(*v);
}

std::string code_of(agentengine::result<std::vector<Capability>> const& r) {
    return r ? std::string("(parsed)") : r.error().code;
}

constexpr std::string_view kFull = R"({
  "fs_read":  [{"mount": "work", "path_prefix": "/src", "size_cap_bytes": 1048576}],
  "fs_write": [{"mount": "work", "path_prefix": "/out", "quota_bytes": 10485760, "file_count_cap": "unbounded"}],
  "net_out":  [{"hosts": ["api.example.com:443:https"], "byte_cap": 1048576, "methods": ["GET"]}],
  "native_exec": [{"program": "node", "worktree_mount": "work", "cpu_ms_cap": 30000, "wall_ms_cap": 30000,
                   "memory_bytes_cap": "unbounded"}]
})";

}  // namespace

int main() {
    // ---- Y1 ------------------------------------------------------------------------------------
    auto full = parse_caps(kFull);
    AE_CHECK(full.has_value(), "Y1: the full document parses");
    if (full) {
        AE_CHECK(full->size() == 4, "Y1: one capability per entry");
        auto const* fr = std::get_if<cap::FsRead>(&(*full)[0]);
        auto const* fw = std::get_if<cap::FsWrite>(&(*full)[1]);
        auto const* net = std::get_if<cap::NetOut>(&(*full)[2]);
        auto const* nx = std::get_if<cap::NativeExec>(&(*full)[3]);
        AE_CHECK(fr && fr->mount_id == "work" && fr->path_prefix == "/src" && fr->size_cap_bytes == cap::Limit::at_most(1048576),
                 "Y1: fs_read fields");
        AE_CHECK(fw && fw->quota_bytes == cap::Limit::at_most(10485760) && fw->file_count_cap.is_unbounded(),
                 "Y1: fs_write fields, `unbounded` stated");
        AE_CHECK(net && net->host_allowlist.size() == 1 && net->byte_cap == cap::Limit::at_most(1048576) &&
                     net->method_restrictions.size() == 1 && net->method_restrictions[0] == "GET",
                 "Y1: net_out fields");
        AE_CHECK(nx && nx->program_pattern == "node" && nx->cpu_ms_cap == cap::Limit::at_most(30000) &&
                     nx->memory_bytes_cap.is_unbounded(),
                 "Y1: native_exec goes through the existing grant parser");
    }

    // ---- Y2 ------------------------------------------------------------------------------------
    if (full) {
        CapabilitySet const from_yaml = CapabilitySet::grant_root(*full);
        CapabilitySet const from_cpp = CapabilitySet::grant_root({
            Capability{cap::FsRead{"work", "/src", cap::Limit::at_most(1048576)}},
            Capability{cap::FsWrite{"work", "/out", cap::Limit::at_most(10485760), cap::Limit::unbounded()}},
            Capability{cap::NetOut{{"api.example.com:443:https"}, cap::Limit::at_most(1048576), {"GET"}}},
        });
        for (auto const& req : {Capability{cap::FsRead{"work", "/src/a", cap::Limit::at_most(1000)}},
                                Capability{cap::FsWrite{"work", "/out", cap::Limit::at_most(10), cap::Limit::at_most(3)}},
                                Capability{cap::NetOut{{"api.example.com:443:https"}, cap::Limit::at_most(5), {"GET"}}}}) {
            AE_CHECK(from_yaml.contains(req) && from_cpp.contains(req),
                     "Y2: the YAML and C++ ceilings both cover the same narrower request");
        }
        AE_CHECK(!from_yaml.contains(Capability{cap::FsRead{"work", "/src", cap::Limit::at_most(2097152)}}),
                 "Y2: a larger fs_read than the document stated is not covered by the ceiling");
        AE_CHECK(!from_yaml.contains(Capability{cap::FsRead{"work", "/src", cap::Limit::unbounded()}}),
                 "Y2: an unbounded fs_read is not covered by a capped ceiling (a widening)");
    }

    // ---- Y3 ------------------------------------------------------------------------------------
    AE_CHECK(code_of(parse_caps(R"({"fs_read":[{"mount":"w","path_prefix":"/"}]})")) == "choice.missing.grant.size_cap_bytes",
             "Y3: an omitted limit is choice.missing.grant.<key>");
    AE_CHECK(code_of(parse_caps(R"({"fs_read":[{"mount":"w","path_prefix":"/","size_cap_bytes":0}]})")) == "choice.zero_bound",
             "Y3: a 0 limit is choice.zero_bound, not unbounded");
    AE_CHECK(parse_caps(R"({"fs_read":[{"mount":"w","path_prefix":"/","size_cap_bytes":"unbounded"}]})").has_value(),
             "Y3: the word `unbounded` is a stated limit");
    AE_CHECK(code_of(parse_caps(R"({"net_out":[{"hosts":["h:1:http"],"byte_cap":5}]})")) == "choice.missing.grant.methods",
             "Y3: omitting `methods` is refused (the word `any` is the stated unrestricted)");

    // ---- Y4 ------------------------------------------------------------------------------------
    AE_CHECK(code_of(parse_caps(R"({"fs_wirte":[{}]})")) == "capabilities.unsupported_kind",
             "Y4: a misspelt kind is refused, not skipped");
    AE_CHECK(code_of(parse_caps(R"({"clock":[{}]})")) == "capabilities.unsupported_kind",
             "Y4: a kind with no document spelling yet is refused");
    AE_CHECK(code_of(parse_caps(R"({"fs_read":[{"mount":"w","path_prefix":"/","size_cap_bytes":5,"extra":1}]})")) == "capabilities.invalid",
             "Y4: an unknown key is refused");
    AE_CHECK(code_of(parse_caps(R"({"fs_read":[{"mount":"w","path_prefix":"","size_cap_bytes":5}]})")) == "capabilities.invalid",
             "Y4: an empty path_prefix is refused (\"/\" is the whole mount)");
    AE_CHECK(code_of(parse_caps(R"({"fs_read":[]})")) == "capabilities.invalid", "Y4: an empty list is refused");
    AE_CHECK(code_of(parse_caps(R"({"fs_read":{"mount":"w"}})")) == "capabilities.invalid",
             "Y4: a kind holding a mapping instead of a list is refused");
    AE_CHECK(code_of(parse_caps(R"(["net_out"])")) == "capabilities.invalid", "Y4: a non-mapping block is refused");
    AE_CHECK(code_of(parse_caps(R"({"net_out":[{"hosts":[],"byte_cap":5,"methods":"any"}]})")) == "capabilities.invalid",
             "Y4: an empty host list is refused (an empty allowlist would read as unrestricted)");

    // ---- Y5 ------------------------------------------------------------------------------------
    AE_CHECK(parse_caps(R"({"net_out":[{"hosts":["h:1:http"],"byte_cap":5,"methods":"any"}]})").has_value(),
             "Y5 positive control: a minimal valid document parses, so Y3/Y4 are not refusing everything");

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "test_capability_yaml: all checks passed\n";
    return 0;
}
