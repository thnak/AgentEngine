#pragma once
// Implements 009-Plugin-and-Extension-System.md §7's "Content reading" catalog entry -- "engine-
// native, not a wrapped library... first on this list because every application needs it on day one
// and it is the candidate most exposed to [the token-budget] hazard -- it ships built-in rather than
// deferred to an operator plugin." First shipped candidate off OpenQuestions.md OQ-17's generic tool
// catalog; a plain `Tool<>` conformer (006 §2's "Native" source), not a WASM plugin -- no production
// plugin loader exists yet anywhere in this repo (`wasm_tool_bridge.hpp`'s own file-top comment,
// ADR-040's residual), so this ships against the seam that already works.
//
// TWO SOURCES -- `url` (`NetOut`-gated, via the real, tested egress mediation `sandbox::
// HostEgressProxy` (ADR-011)) and, as of this pass, `path` (`FsRead`-gated, via `EffectContext::
// sandbox_fs`, the session's own mediated sandbox mount). Exactly one of the two must be set --
// `invoke()` rejects both and neither. The `path` source follows `tools/read_sandbox_file.hpp`'s
// own proven pattern exactly (that file's own comment named this exact extension as deferred work
// once `EffectContext::sandbox_fs` existed for real, which it now does -- `session_shell_wiring.hpp`/
// `SandboxToolProvider`, ADR-096 Design B, already populates it in production). `read_sandbox_file`
// itself is left in place, not removed by this pass -- it was a worked example proving the seam, and
// deciding whether it is now fully redundant is a separate call this pass doesn't make.
//
// Threshold/blob-promotion (006 §7) is identical for both sources -- `read_content_detail::
// build_reply` below is source-agnostic, operating on already-fetched bytes regardless of where they
// came from.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::tools {

// A parsed `http(s)://host[:port]/path[?query]` URL. Deliberately minimal -- there is no vetted URL
// library to lean on any more than there was an HTTP one (ADR-011 §2's own stated reason for hand-
// rolling `net_egress_proxy.cpp`'s request framing), and this tool only ever needs the four fields a
// `NetOut` grant lookup plus a `NetEgressRequest` require. No IPv6 literal-host support (`[::1]`) --
// out of scope for the identical reason `net_egress_proxy.cpp`'s own "host:port:scheme" allowlist
// grammar has never needed it either.
// ae-naming-lint: allow ParsedUrl — a minimal hand-rolled URL split, not a vetted library type; 027 not yet updated
struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
    std::string path;  // always starts with '/'; carries any '?query' verbatim
};

[[nodiscard]] inline result<ParsedUrl> parse_read_content_url(std::string_view url) {
    auto const scheme_sep = url.find("://");
    if (scheme_sep == std::string_view::npos) {
        return std::unexpected(error{failure_class::contract,
                                      "read_content url must start with 'http://' or 'https://'",
                                      "read_content.malformed_url"});
    }
    std::string_view const scheme = url.substr(0, scheme_sep);
    if (scheme != "http" && scheme != "https") {
        return std::unexpected(error{failure_class::contract,
                                      "read_content only supports http/https urls",
                                      "read_content.scheme_unsupported"});
    }

    std::string_view const rest = url.substr(scheme_sep + 3);
    std::size_t const path_start = rest.find('/');
    std::string_view const authority = (path_start == std::string_view::npos) ? rest : rest.substr(0, path_start);
    std::string path = (path_start == std::string_view::npos) ? "/" : std::string(rest.substr(path_start));
    if (authority.empty()) {
        return std::unexpected(error{failure_class::contract, "read_content url is missing a host",
                                      "read_content.malformed_url"});
    }

    std::string host;
    std::uint16_t port = (scheme == "https") ? 443 : 80;
    std::size_t const port_sep = authority.rfind(':');
    if (port_sep != std::string_view::npos) {
        std::string_view const port_sv = authority.substr(port_sep + 1);
        std::uint16_t parsed_port = 0;
        auto const [ptr, ec] = std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), parsed_port);
        if (!port_sv.empty() && ec == std::errc{} && ptr == port_sv.data() + port_sv.size()) {
            host = std::string(authority.substr(0, port_sep));
            port = parsed_port;
        } else {
            host = std::string(authority);  // no real ":port" suffix -- treat the whole thing as host
        }
    } else {
        host = std::string(authority);
    }
    if (host.empty()) {
        return std::unexpected(error{failure_class::contract, "read_content url is missing a host",
                                      "read_content.malformed_url"});
    }

    return ParsedUrl{std::string(scheme), std::move(host), port, std::move(path)};
}

// "host:port:scheme" -- `cap::NetOut::host_allowlist`'s own grammar (`net_egress_proxy.cpp`'s
// `parse_allowlist_entry`), so a granted entry can be matched against a parsed URL by exact string.
[[nodiscard]] inline std::string net_out_target_string(ParsedUrl const& url) {
    return url.host + ":" + std::to_string(url.port) + ":" + url.scheme;
}

namespace read_content_detail {

[[nodiscard]] inline bool equals_ci(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                       [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

// A fallback used ONLY when the caller hasn't wired `ctx.tool_result_byte_threshold` at all (`006
// §7`'s "scaled to the run's effective per-turn budget" has no signal to scale from in that case).
// Not itself an instance of the "fixed byte constant" anti-pattern §7 rejects -- that rule is about
// the SCALED case; this is the floor applied when no scaling signal exists, chosen so an unbounded
// fetch can never inline its entire body regardless of whether a caller remembered to wire a real
// budget hint. ~2000 tokens by `context_assembly.hpp`'s own established 4-bytes-per-token ratio.
inline constexpr std::uint64_t kDefaultPreviewByteCap = 8000;

// The `path` source's sandbox mount id. Scoped inside this detail namespace (not
// `agentengine::tools`) specifically so it can never collide with `read_sandbox_file.hpp`'s own
// `kSandboxWorkMount` if both headers end up included in the same translation unit -- same mount,
// deliberately independent symbol.
inline constexpr std::string_view kWorkMount = "work";

}  // namespace read_content_detail

// ae-naming-lint: allow ReadContentArgs — new tool, matches every other Args type's own naming
struct ReadContentArgs {
    // Exactly one of the two must be set (`invoke()` rejects both and neither).
    std::optional<std::string> url;
    std::optional<std::string> path;  // relative to this session's "work" sandbox mount
};
AE_JSON_SCHEMA(ReadContentArgs, url, path)

// ae-naming-lint: allow ReadContentReply — new tool, matches every other Reply type's own naming
struct ReadContentReply {
    std::string preview;                // bounded; the whole body when `truncated` is false
    bool truncated = false;             // true iff `preview` is not the entire fetched body
    std::uint64_t total_bytes = 0;
    std::string media_type;
    std::optional<BlobRef> blob;        // set iff `truncated` AND a blob sink was available (006 §7)
};
AE_JSON_SCHEMA(ReadContentReply, preview, truncated, total_bytes, media_type, blob)

namespace read_content_detail {

// Source-agnostic threshold/blob-promotion (006 §7) -- identical decision for a URL fetch's body or
// a sandbox file's bytes, so both `invoke_via` (URL) and `invoke_from_sandbox` (path) below funnel
// through this one function rather than duplicating the threshold/truncate/blob-sink logic per
// source. Moved here verbatim from the pre-`path`-source `ReadContent::invoke_via` body.
[[nodiscard]] inline result<ReadContentReply> build_reply(std::string&& body, std::string media_type,
                                                           EffectContext& ctx) {
    ReadContentReply reply;
    reply.total_bytes = body.size();
    reply.media_type = std::move(media_type);

    std::uint64_t const threshold = ctx.tool_result_byte_threshold.value_or(kDefaultPreviewByteCap);

    if (reply.total_bytes <= threshold) {
        reply.preview = std::move(body);
        reply.truncated = false;
        return reply;
    }

    // Over threshold: the preview is always bounded (never the hazard 006 §7 exists to prevent)
    // regardless of whether a blob sink is available to also hand back the rest -- a caller with
    // no sink wired still gets useful, safely-truncated content instead of a hard failure; only
    // the pipeline-level generic promotion (`tool_pipeline.hpp`'s own `normalize_success`, which
    // has no smaller field to fall back to) fails closed with no sink.
    reply.preview = body.substr(0, static_cast<std::size_t>(threshold));
    reply.truncated = true;
    if (ctx.blob_sink) {
        std::span<std::byte const> const bytes{reinterpret_cast<std::byte const*>(body.data()),
                                                 body.size()};
        auto blob = ctx.blob_sink(bytes, reply.media_type);
        if (!blob) return std::unexpected(blob.error());
        reply.blob = *blob;
    }
    return reply;
}

}  // namespace read_content_detail

// 009 §7's `read_content` candidate. Declares an EMPTY static capability ceiling deliberately --
// `Capabilities<NetOut<"...">>`/`Capabilities<FsRead<"...">>`'s compile-time forms each fix ONE
// target per tool TYPE, but this tool must work against whatever host or path the model's `url`/
// `path` argument names and whatever grants the run actually holds, which is a per-CALL question
// `Tool`'s static declaration surface (006 §1) cannot express for either source. Enforcement instead
// happens dynamically inside `invoke()` via `ctx.capabilities->find_net_out(...)` / `find_fs_read(
// ...)` -- the same "real, path/host-scoped capability check against `EffectContext::capabilities`,
// not merely the tool's own static ceiling" pattern `mediated_shell_dispatch.hpp`'s own file-top
// comment already documents and this codebase already ships for the identical reason (a generic
// dispatcher whose target isn't known until call time). This is still I2-compliant: authority is
// still gated by an explicit, pre-granted capability, checked before any effect -- only the pipeline
// STEP that checks it moves from 4 (generic, static) to inside `invoke()` (specific, dynamic);
// nothing here ever grants itself anything.
// ae-naming-lint: allow ReadContent — 009 §7's read_content candidate tool; 027 not yet updated
struct ReadContent : Tool<ReadContent, Capabilities<>, Approval<approval_mode::never_require>,
                           EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "read_content";
    static constexpr std::string_view description =
        "Read content from a URL under one of this run's granted network-egress hosts, or from a "
        "path in this session's own sandbox mount under one of this run's granted read capabilities. "
        "Set exactly one of 'url' or 'path'. Returns a bounded preview; content above the run's size "
        "threshold is promoted to a blob the caller can open separately rather than being inlined "
        "whole.";

    using Args = ReadContentArgs;
    using Reply = ReadContentReply;

    // The real entry point -- `Tool<>`'s fixed `invoke(Args, EffectContext&)` signature (006 §1).
    // Dispatches on which of `url`/`path` is set; rejects the ambiguous cases (both or neither)
    // before either source-specific path runs. The `url` branch always supplies the real,
    // production `sandbox::HostEgressProxy` (default resolver, the real SSRF blocklist
    // `resolve_and_validate` enforces) -- delegates to `invoke_via` below.
    [[nodiscard]] static result<Reply> invoke(Args args, EffectContext& ctx) {
        bool const has_url = args.url.has_value();
        bool const has_path = args.path.has_value();
        if (has_url == has_path) {
            return std::unexpected(error{failure_class::contract,
                                          "read_content requires exactly one of 'url' or 'path'",
                                          "read_content.ambiguous_source"});
        }
        if (has_path) return invoke_from_sandbox(args, ctx);
        return invoke_via(args, ctx, sandbox::HostEgressProxy{});
    }

    // The `path` source: reads one file from the session's own mediated sandbox mount
    // (`EffectContext::sandbox_fs`), the same seam and the same dynamic-capability-check pattern
    // `tools/read_sandbox_file.hpp` already proved end to end. No `Content-Type` header exists for
    // a local file, so `media_type` is always the same generic default `invoke_via` below falls
    // back to when a URL response omits one.
    [[nodiscard]] static result<Reply> invoke_from_sandbox(Args const& args, EffectContext& ctx) {
        if (!ctx.sandbox_fs) {
            return std::unexpected(error{failure_class::resource,
                                          "this session has no sandbox mount yet",
                                          "read_content.no_sandbox"});
        }
        if (!ctx.capabilities) {
            return std::unexpected(error{failure_class::policy, "no capability set bound to this call",
                                          "tool.capability_not_held"});
        }
        std::string const mount{read_content_detail::kWorkMount};
        auto granted = ctx.capabilities->find_fs_read(mount, *args.path);
        if (!granted) {
            return std::unexpected(error{failure_class::policy,
                                          "no granted FsRead capability covers '" + *args.path +
                                              "' on the '" + mount + "' mount",
                                          "tool.capability_not_held"});
        }

        auto bytes = ctx.sandbox_fs->read_file(*args.path);
        if (!bytes) return std::unexpected(bytes.error());

        std::string body(reinterpret_cast<char const*>(bytes->data()), bytes->size());
        return read_content_detail::build_reply(std::move(body), "application/octet-stream", ctx);
    }

    // The actual `url`-source logic, parameterized over the egress backend (`sandbox::
    // NetEgressBackend`, net_egress_proxy.hpp). `invoke()` above always supplies the real
    // `HostEgressProxy`; a test supplies a fake conformer instead -- the SAME testability seam
    // `HostEgressProxy::resolver`'s own comment already documents ("a test may inject a fake...
    // production code never constructs a HostEgressProxy with a non-default resolver"), applied
    // one level up so this function's fetch/error-mapping logic is provable without a live network
    // call or tripping the (correct, intentional) loopback/RFC1918 block a real local test server
    // would hit.
    template <sandbox::NetEgressBackend Proxy>
    [[nodiscard]] static result<Reply> invoke_via(Args const& args, EffectContext& ctx, Proxy const& proxy) {
        auto parsed = parse_read_content_url(*args.url);
        if (!parsed) return std::unexpected(parsed.error());

        if (!ctx.capabilities) {
            return std::unexpected(error{failure_class::policy, "no capability set bound to this call",
                                          "tool.capability_not_held"});
        }
        std::string const target = net_out_target_string(*parsed);
        auto granted = ctx.capabilities->find_net_out(target);
        if (!granted) {
            return std::unexpected(error{failure_class::policy,
                                          "no granted NetOut capability covers '" + target + "'",
                                          "tool.capability_not_held"});
        }

        sandbox::NetEgressRequest const req{"GET", parsed->path, {}, {}};
        auto response = proxy.fetch(req, *granted);
        if (!response) return std::unexpected(response.error());
        if (response->status < 200 || response->status >= 300) {
            return std::unexpected(error{failure_class::transient,
                                          "read_content received HTTP status " +
                                              std::to_string(response->status),
                                          "read_content.http_error"});
        }

        std::string media_type = "application/octet-stream";
        for (auto const& [k, v] : response->headers) {
            if (read_content_detail::equals_ci(k, "content-type")) {
                media_type = v;
                break;
            }
        }

        return read_content_detail::build_reply(std::move(response->body), std::move(media_type), ctx);
    }
};

}  // namespace agentengine::tools
