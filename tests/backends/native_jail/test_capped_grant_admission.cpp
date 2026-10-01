// Proof for decisions/ADR-217-capped-grant-admits-grant-enforcing-tools.md (GitHub #148): a quota- or
// size-capped FsRead/FsWrite grant admits a tool that declares an uncapped ceiling AND
// `EnforcesGrantedCaps`, the per-call handle carries the GRANT's cap, and the cap still holds at use.
//   U1-U9  -- CapabilitySet::clamped_binding()/bind_clamped(): clamps quantity axes only, never scope
//             axes; contains()/bind()/attenuate() unchanged.
//   P1-P5  -- through the real invoke_tool pipeline with a probe tool.
//   R1-R4  -- the real run_shell (SessionShellSandbox) under a capped grant: admitted, and the shell's
//             own gap-12 lookups enforce the size cap and the quota at use.
//   Q1-Q2  -- policy reachability reports what the runtime admits.
//   A1-A3  -- the agent registry's ceiling check uses the same rule.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/agent_registry.hpp"
#include "agentengine/trust/policy_reachability.hpp"
#include "backends/native_jail/session_shell_wiring.hpp"

namespace {

namespace ae   = agentengine;
namespace json = agentengine::json;
using ae::Capability;
using ae::CapabilitySet;

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

constexpr std::uint64_t kReadCap  = 1u << 20;
constexpr std::uint64_t kQuota    = 16u << 20;
constexpr std::uint32_t kFileCap  = 1024;

CapabilitySet capped_grant(std::string const& mount = "work", std::string const& prefix = "") {
    return CapabilitySet::grant_root({Capability{ae::cap::FsRead{mount, prefix, kReadCap}},
                                      Capability{ae::cap::FsWrite{mount, prefix, kQuota, kFileCap}}});
}

struct ProbeArgs {
    bool noop;
};
AE_JSON_SCHEMA(ProbeArgs, noop)
struct ProbeReply {
    bool ok;
};
AE_JSON_SCHEMA(ProbeReply, ok)

// What each probe call saw in its bound handles.
std::vector<Capability> g_seen;
int                     g_invocations = 0;


ae::result<ProbeReply> record_bound(ae::EffectContext& ctx) {
    ++g_invocations;
    g_seen.clear();
    if (ctx.bound_capabilities) {
        for (ae::BoundCapability const& b : *ctx.bound_capabilities) {
            if (auto c = b.use()) g_seen.push_back(*c);
        }
    }
    return ProbeReply{true};
}

struct GrantEnforcingProbe
    : ae::Tool<GrantEnforcingProbe,
               ae::Capabilities<ae::cap::decl::FsRead<"work">, ae::cap::decl::FsWrite<"work">>,
               ae::EnforcesGrantedCaps> {
    static constexpr std::string_view name        = "grant_enforcing_probe";
    static constexpr std::string_view description = "Records its bound capabilities.";
    using Args  = ProbeArgs;
    using Reply = ProbeReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext& ctx) { return record_bound(ctx); }
};

struct PlainProbe
    : ae::Tool<PlainProbe, ae::Capabilities<ae::cap::decl::FsRead<"work">, ae::cap::decl::FsWrite<"work">>> {
    static constexpr std::string_view name        = "plain_probe";
    static constexpr std::string_view description = "Records its bound capabilities.";
    using Args  = ProbeArgs;
    using Reply = ProbeReply;
    static ae::result<Reply> invoke(Args, ae::EffectContext& ctx) { return record_bound(ctx); }
};

ae::EffectContext make_ctx(CapabilitySet const& held) {
    ae::EffectContext ctx;
    ctx.principal    = ae::Principal{"test-principal", ""};
    ctx.capabilities = ae::borrow_capabilities(held);
    return ctx;
}

// The last call's audited error code (the model-facing Error carries only a message).
std::string g_last_code;

std::string error_code(ae::ToolResult const& r) { return r.is_error ? g_last_code : ""; }

// The whole visible outcome of a run_shell call, error or reply, for substring checks.
std::string outcome_text(ae::ToolResult const& r) {
    if (r.content.empty()) return "";
    if (auto const* e = std::get_if<ae::Error>(&r.content[0].value)) return "ERROR " + g_last_code + " " + e->message;
    if (auto const* d = std::get_if<ae::Data>(&r.content[0].value)) return d->json;
    return "";
}

ae::ToolResult call(ae::ToolTable const& table, CapabilitySet const& held, std::string const& tool,
                    json::Value args, ae::FileSystemAdapter* fs = nullptr) {
    auto ctx       = make_ctx(held);
    ctx.sandbox_fs = fs;
    ae::ToolCallRequest const req{"call", tool, std::move(args), false};
    ae::ToolInvocationAudit audit;
    auto r      = ae::invoke_tool(table, held, req, ctx, nullptr, &audit);
    g_last_code = audit.error_code;
    return r;
}

json::Value probe_args() { return *json::parse(R"({"noop":true})"); }
json::Value shell(std::string const& source) {
    return json::Value::make_object({{"source", json::Value::make_string(source)}});
}

}  // namespace

int main() {
    Capability const uncapped_read{ae::cap::FsRead{"work", "", std::nullopt}};
    Capability const uncapped_write{ae::cap::FsWrite{"work", "", std::nullopt, std::nullopt}};

    // ---- U: the primitive ----------------------------------------------------------------------
    {
        CapabilitySet const held = capped_grant();
        auto r = held.clamped_binding(uncapped_read);
        auto const* fr = r ? std::get_if<ae::cap::FsRead>(&*r) : nullptr;
        check(fr && fr->mount_id == "work" && fr->path_prefix.empty() && fr->size_cap_bytes == kReadCap,
              "U1: an uncapped FsRead requirement binds under a capped grant WITH the grant's size cap");
        auto w = held.clamped_binding(uncapped_write);
        auto const* fw = w ? std::get_if<ae::cap::FsWrite>(&*w) : nullptr;
        check(fw && fw->quota_bytes == kQuota && fw->file_count_cap == kFileCap,
              "U2: an uncapped FsWrite requirement binds with the grant's quota and file-count cap");

        check(!held.contains(uncapped_read) && !held.bind(uncapped_read).has_value(),
              "U3: contains()/bind() are unchanged -- still refuse an uncapped request under a capped grant");
        check(!held.attenuate({uncapped_read}).has_value(),
              "U4: attenuate() is unchanged -- a capped grant can never be attenuated into an uncapped one");

        check(!held.clamped_binding(Capability{ae::cap::FsRead{"other", "", std::nullopt}}).has_value(),
              "U5: a requirement on a different mount is still refused");
        CapabilitySet const prefixed = capped_grant("work", "notes");
        check(!prefixed.clamped_binding(uncapped_read).has_value(),
              "U5: a requirement for the mount root under a grant scoped to a sub-prefix is still refused");
        check(prefixed.clamped_binding(Capability{ae::cap::FsRead{"work", "notes/a", std::nullopt}}).has_value(),
              "U5 (control): a requirement inside the granted prefix is admitted");

        auto tight = held.clamped_binding(Capability{ae::cap::FsRead{"work", "", 10u}});
        check(tight && std::get<ae::cap::FsRead>(*tight).size_cap_bytes == 10u,
              "U6: a requirement tighter than the grant keeps its own (tighter) cap");
        auto loose = held.clamped_binding(Capability{ae::cap::FsRead{"work", "", kReadCap * 4}});
        check(loose && std::get<ae::cap::FsRead>(*loose).size_cap_bytes == kReadCap,
              "U6: a requirement looser than the grant is lowered to the grant's cap");

        CapabilitySet const open = CapabilitySet::grant_root({uncapped_read});
        auto same = open.clamped_binding(uncapped_read);
        check(same && std::get<ae::cap::FsRead>(*same).size_cap_bytes == std::nullopt,
              "U7: under an uncapped grant the binding is the requirement itself (old behaviour)");

        // Scope axes of other kinds stay strict.
        CapabilitySet const net = CapabilitySet::grant_root(
            {Capability{ae::cap::NetOut{{"api.example.com:443:https"}, 4096u, {"GET"}}}});
        auto n = net.clamped_binding(Capability{ae::cap::NetOut{{"api.example.com:443:https"}, std::nullopt, {"GET"}}});
        check(n && std::get<ae::cap::NetOut>(*n).byte_cap == 4096u,
              "U8: NetOut byte_cap is a quantity axis -- clamped to the grant's");
        check(!net.clamped_binding(Capability{ae::cap::NetOut{{"evil.example.com:443:https"}, std::nullopt, {"GET"}}})
                   .has_value(),
              "U8: a host the grant does not list is still refused");
        check(!net.clamped_binding(Capability{ae::cap::NetOut{{"api.example.com:443:https"}, std::nullopt, {}}})
                   .has_value(),
              "U8: an unrestricted method list under a GET-only grant is still refused (scope, not quantity)");

        // Several grants: a capped grant whose scope does not cover is skipped, the covering one is used.
        CapabilitySet const two = CapabilitySet::grant_root({Capability{ae::cap::FsRead{"work", "notes", 5u}},
                                                             Capability{ae::cap::FsRead{"work", "", 7u}}});
        auto pick = two.clamped_binding(uncapped_read);
        check(pick && std::get<ae::cap::FsRead>(*pick).size_cap_bytes == 7u &&
                  std::get<ae::cap::FsRead>(*pick).path_prefix.empty(),
              "U9: the binding comes from the grant whose scope covers the requirement");
    }

    // ---- P: through invoke_tool ----------------------------------------------------------------
    {
        auto const table = ae::ToolTable::from_tools<GrantEnforcingProbe, PlainProbe>();
        CapabilitySet const held = capped_grant();

        g_invocations = 0;
        auto r1 = call(table, held, "grant_enforcing_probe", probe_args());
        check(!r1.is_error && g_invocations == 1,
              "P1: a capped grant admits a tool that declares uncapped FsRead/FsWrite and EnforcesGrantedCaps");
        bool carries_grant_caps = g_seen.size() == 2;
        if (carries_grant_caps) {
            auto const* fr = std::get_if<ae::cap::FsRead>(&g_seen[0]);
            auto const* fw = std::get_if<ae::cap::FsWrite>(&g_seen[1]);
            carries_grant_caps = fr && fw && fr->size_cap_bytes == kReadCap && fw->quota_bytes == kQuota &&
                                 fw->file_count_cap == kFileCap;
        }
        check(carries_grant_caps,
              "P1: the bound handles the tool receives carry the GRANT's caps, not its uncapped declaration");

        g_invocations = 0;
        auto r2 = call(table, held, "plain_probe", probe_args());
        check(r2.is_error && error_code(r2) == "tool.capability_not_held" && g_invocations == 0,
              "P2 (positive control): the same ceiling WITHOUT EnforcesGrantedCaps is still refused before invoke");

        g_invocations = 0;
        auto r3 = call(table, capped_grant("other"), "grant_enforcing_probe", probe_args());
        check(r3.is_error && error_code(r3) == "tool.capability_not_held" && g_invocations == 0,
              "P3: a capped grant on a different mount still refuses the tool");

        g_invocations = 0;
        auto r4 = call(table, capped_grant("work", "notes"), "grant_enforcing_probe", probe_args());
        check(r4.is_error && error_code(r4) == "tool.capability_not_held" && g_invocations == 0,
              "P4: a capped grant on a narrower path prefix still refuses a mount-root declaration");

        CapabilitySet const open = CapabilitySet::grant_root({uncapped_read, uncapped_write});
        g_invocations = 0;
        auto r5 = call(table, open, "plain_probe", probe_args());
        bool uncapped_bound = g_seen.size() == 2 &&
                              std::get_if<ae::cap::FsRead>(&g_seen[0]) &&
                              !std::get<ae::cap::FsRead>(g_seen[0]).size_cap_bytes.has_value();
        check(!r5.is_error && g_invocations == 1 && uncapped_bound,
              "P5: an uncapped grant admits the plain tool exactly as before, bound uncapped");
    }

    // ---- R: the real run_shell, cap enforced at use --------------------------------------------
    {
        std::filesystem::path const scratch =
            std::filesystem::temp_directory_path() / "ae_capped_grant_admission_test";
        std::filesystem::remove_all(scratch);
        std::filesystem::create_directories(scratch);
        auto sandbox = ae::SessionShellSandbox::create(scratch);
        check(sandbox.has_value(), "setup: SessionShellSandbox::create succeeds");
        if (!sandbox) return 1;

        ae::ToolDescriptor const shell_tool = (*sandbox)->tool_descriptor();
        check(shell_tool.enforces_granted_caps, "setup: run_shell's live descriptor carries EnforcesGrantedCaps");
        auto const table = ae::ToolTable::from_descriptors({shell_tool});

        constexpr std::uint64_t kSmallRead  = 16;
        constexpr std::uint64_t kSmallQuota = 64;
        CapabilitySet const held = CapabilitySet::grant_root(
            {Capability{ae::cap::FsRead{"work", "", kSmallRead}},
             Capability{ae::cap::FsWrite{"work", "", kSmallQuota, 100u}}});
        ae::FileSystemAdapter* fs = (*sandbox)->filesystem_adapter();

        auto r1 = call(table, held, "run_shell", shell("echo hi > small.txt"), fs);
        check(!r1.is_error && std::filesystem::exists(scratch / "small.txt"),
              "R1: run_shell is admitted under a size-capped, quota-capped grant and its write lands");

        {
            std::ofstream big(scratch / "big.txt", std::ios::binary);
            big << std::string(200, 'x');  // over the 16-byte read cap AND the 64-byte quota
        }
        auto r2 = call(table, held, "run_shell", shell("cat big.txt"), fs);
        std::string const t2 = outcome_text(r2);
        check(t2.find("size cap") != std::string::npos && t2.find(std::string(50, 'x')) == std::string::npos,
              "R2: the grant's 16-byte size cap is enforced at use -- cat of a 200-byte file is refused");

        auto r3 = call(table, held, "run_shell", shell("echo more > after.txt"), fs);
        std::string const t3 = outcome_text(r3);
        check(t3.find("No space left on device") != std::string::npos &&
                  !std::filesystem::exists(scratch / "after.txt"),
              "R3: the grant's 64-byte quota is enforced at use -- a write over quota is refused");

        ae::ToolDescriptor untagged = shell_tool;
        untagged.enforces_granted_caps = false;
        auto const untagged_table = ae::ToolTable::from_descriptors({untagged});
        auto r4 = call(untagged_table, held, "run_shell", shell("echo x > never.txt"), fs);
        check(r4.is_error && error_code(r4) == "tool.capability_not_held" &&
                  !std::filesystem::exists(scratch / "never.txt"),
              "R4 (positive control, the #148 bug): without the tag the capped grant refuses run_shell");

        sandbox->reset();
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
    }

    // ---- Q: policy reachability mirrors the runtime ---------------------------------------------
    {
        std::vector<Capability> const ceiling{Capability{ae::cap::FsRead{"work", "", kReadCap}},
                                              Capability{ae::cap::FsWrite{"work", "", kQuota, kFileCap}}};
        ae::trust::ReachabilityAgent const agent{
            "agent", ceiling,
            {ae::make_tool_descriptor<GrantEnforcingProbe>(), ae::make_tool_descriptor<PlainProbe>()}};
        auto const report = ae::trust::enumerate_policy_reachability({agent}, {});
        bool tagged_granted = true, tagged_seen = false, plain_denied = true, plain_seen = false;
        for (auto const& cell : report.cells) {
            if (cell.tool_name == "grant_enforcing_probe") {
                tagged_seen = true;
                if (!cell.granted) tagged_granted = false;
            } else if (cell.tool_name == "plain_probe") {
                plain_seen = true;
                if (cell.granted) plain_denied = false;
            }
        }
        check(tagged_seen && tagged_granted,
              "Q1: reachability reports the grant-enforcing tool reachable under a capped ceiling (as admitted)");
        check(plain_seen && plain_denied, "Q2: and the plain tool unreachable (as refused)");
    }

    // ---- A: the agent registry's ceiling check uses the same rule ---------------------------------
    {
        CapabilitySet const ceiling = capped_grant();
        auto const tagged = ae::agent_detail::check_capability_ceiling(ceiling, {ae::make_tool_descriptor<GrantEnforcingProbe>()});
        check(tagged.has_value(), "A1: an agent with a capped ceiling may list a grant-enforcing tool");
        auto const plain = ae::agent_detail::check_capability_ceiling(ceiling, {ae::make_tool_descriptor<PlainProbe>()});
        check(!plain.has_value() && plain.error().code == "agent.capability_ceiling_exceeded",
              "A2: and is still refused a plain tool declaring the uncapped ceiling");
        auto const other = ae::agent_detail::check_capability_ceiling(capped_grant("other"),
                                                        {ae::make_tool_descriptor<GrantEnforcingProbe>()});
        check(!other.has_value(), "A3: a ceiling on a different mount still refuses the grant-enforcing tool");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "All checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
