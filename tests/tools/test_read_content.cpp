// Proves tools/read_content.hpp's ReadContent tool -- Track A/B of the first-party-tools scope
// (docs/research/2026-08-24-dify-ai-feature-comparison.md's named gap; 009 §7's "Content reading"
// catalog entry). Exercises `invoke_via` against a fake `sandbox::NetEgressBackend` conformer, the
// same testability seam `HostEgressProxy::resolver`'s own comment documents (a fake backend, not a
// live network call or the intentional loopback/RFC1918 block a real local test server would trip) --
// `invoke()` itself (always the real `HostEgressProxy`) is exercised indirectly by
// `test_net_egress_proxy.cpp`'s own suite for `HostEgressProxy::fetch`'s real composition; this file
// proves ReadContent's OWN logic: URL parsing, the dynamic NetOut capability check, threshold/blob
// promotion, and error mapping for the `url` source, plus (Track B) the `path` source's dynamic
// FsRead capability check against a fake `FileSystemAdapter` conformer -- the same "fake, not real
// I/O" pattern `FakeEgressBackend` below already establishes; `test_effect_context_sandbox_fs.cpp`
// proves `path` against a REAL `MediatedFileSystemAdapter` (Windows-only there), so this file's own
// `path`-source tests deliberately stay portable and stick to what's ReadContent's own logic: source
// dispatch/ambiguity, the capability check, and reusing `build_reply`'s threshold/blob-promotion.

#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "agentengine/tools/read_content.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using agentengine::result;
namespace sandbox = agentengine::sandbox;

struct FakeEgressBackend {
    static constexpr sandbox::NetEgressTraits traits{"fake-egress", true};

    result<sandbox::NetEgressResponse> response{sandbox::NetEgressResponse{200, {}, "hello"}};
    mutable int fetch_calls = 0;
    mutable std::optional<sandbox::NetEgressRequest> last_request;
    mutable std::optional<agentengine::cap::NetOut> last_granted;

    [[nodiscard]] result<sandbox::NetEgressResponse> fetch(sandbox::NetEgressRequest const& req,
                                                              agentengine::cap::NetOut const& granted) const {
        ++fetch_calls;
        last_request = req;
        last_granted = granted;
        return response;
    }
};
static_assert(sandbox::NetEgressBackend<FakeEgressBackend>);

// Fake `FileSystemAdapter` for the `path` source -- mirrors `FakeEgressBackend` above and
// `test_effect_context_sandbox_fs.cpp`'s `CountingFileSystemAdapter`, but with a settable
// `read_file` result so this file's own threshold/blob-promotion tests don't need a real mount.
class FakeFileSystemAdapter final : public agentengine::FileSystemAdapter {
public:
    agentengine::result<std::vector<std::byte>> response{std::vector<std::byte>{}};
    mutable int read_file_calls = 0;
    mutable std::optional<std::string> last_path;

    agentengine::result<std::vector<std::byte>> read_file(std::string_view path) override {
        ++read_file_calls;
        last_path = std::string(path);
        return response;
    }
    agentengine::result<void> write_file(std::string_view, std::span<std::byte const>, bool) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<void> remove(std::string_view, bool) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<void> rename(std::string_view, std::string_view) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<void> copy_file(std::string_view, std::string_view) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<void> make_directory(std::string_view, bool) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<std::vector<agentengine::DirEntry>> list_directory(std::string_view) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract, "not implemented",
                                                    "test.not_implemented"});
    }
    agentengine::result<bool> exists(std::string_view) override { return true; }
    agentengine::result<std::string> canonicalize(std::string_view path) override {
        return std::string(path);
    }
};

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view s) {
    return std::vector<std::byte>(reinterpret_cast<std::byte const*>(s.data()),
                                   reinterpret_cast<std::byte const*>(s.data() + s.size()));
}

agentengine::EffectContext make_ctx(agentengine::CapabilitySet const& held) {
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);
    return ctx;
}

void test_url_parsing() {
    using agentengine::tools::parse_read_content_url;

    {
        auto p = parse_read_content_url("http://example.com/foo/bar?x=1");
        check(p.has_value(), "plain http url with path+query parses");
        if (p) {
            check(p->scheme == "http", "scheme is http");
            check(p->host == "example.com", "host parsed");
            check(p->port == 80, "http defaults to port 80");
            check(p->path == "/foo/bar?x=1", "path+query preserved verbatim");
        }
    }
    {
        auto p = parse_read_content_url("https://example.com:8443");
        check(p.has_value(), "https url with explicit port and no path parses");
        if (p) {
            check(p->port == 8443, "explicit port honored");
            check(p->path == "/", "missing path defaults to '/'");
        }
    }
    {
        auto p = parse_read_content_url("https://example.com");
        check(p.has_value() && p->port == 443, "https defaults to port 443");
    }
    {
        auto p = parse_read_content_url("ftp://example.com/x");
        check(!p.has_value() && p.error().code == "read_content.scheme_unsupported",
              "non-http(s) scheme is rejected");
    }
    {
        auto p = parse_read_content_url("not-a-url-at-all");
        check(!p.has_value() && p.error().code == "read_content.malformed_url",
              "url with no scheme separator is rejected");
    }
    {
        auto p = parse_read_content_url("http:///no-host");
        check(!p.has_value(), "url with an empty host is rejected");
    }
}

void test_happy_path_under_threshold() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::NetOut{{"example.com:80:http"}, std::nullopt, {}}});
    auto ctx = make_ctx(held);

    FakeEgressBackend backend;
    backend.response = sandbox::NetEgressResponse{200, {{"Content-Type", "text/plain"}}, "hello world"};

    auto reply = ReadContent::invoke_via(ReadContent::Args{.url = "http://example.com/data", .path = std::nullopt}, ctx, backend);
    check(reply.has_value(), "happy path succeeds");
    if (reply) {
        check(reply->preview == "hello world", "preview is the whole body under threshold");
        check(!reply->truncated, "not truncated under threshold");
        check(!reply->blob.has_value(), "no blob promoted under threshold");
        check(reply->total_bytes == 11, "total_bytes matches body size");
        check(reply->media_type == "text/plain", "media_type read from Content-Type header");
    }
    check(backend.fetch_calls == 1, "backend.fetch called exactly once");
    if (backend.last_request) {
        check(backend.last_request->method == "GET", "request method is always GET");
        check(backend.last_request->path == "/data", "request path matches the parsed url");
    }
    if (backend.last_granted) {
        check(backend.last_granted->host_allowlist.size() == 1 &&
                  backend.last_granted->host_allowlist[0] == "example.com:80:http",
              "the narrowed single-host NetOut is what reaches the backend");
    }
}

void test_over_threshold_with_blob_sink() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::NetOut{{"example.com:80:http"}, std::nullopt, {}}});
    auto ctx = make_ctx(held);
    ctx.tool_result_byte_threshold = 5;

    std::string const full_body(50, 'x');
    FakeEgressBackend backend;
    backend.response = sandbox::NetEgressResponse{200, {}, full_body};

    std::string sunk_bytes;
    std::string sunk_media_type;
    ctx.blob_sink = [&](std::span<std::byte const> bytes, std::string const& media_type) -> result<agentengine::BlobRef> {
        sunk_bytes.assign(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        sunk_media_type = media_type;
        return agentengine::BlobRef{"deadbeef", media_type, bytes.size(), "test-store"};
    };

    auto reply = ReadContent::invoke_via(ReadContent::Args{.url = "http://example.com/big", .path = std::nullopt}, ctx, backend);
    check(reply.has_value(), "over-threshold call still succeeds when a sink is wired");
    if (reply) {
        check(reply->truncated, "flagged truncated over threshold");
        check(reply->preview.size() == 5, "preview is bounded to the threshold, not the whole body");
        check(reply->preview == full_body.substr(0, 5), "preview is a prefix of the real body");
        check(reply->total_bytes == 50, "total_bytes reports the REAL size, not the preview's");
        check(reply->blob.has_value(), "a blob is promoted over threshold when a sink exists");
        if (reply->blob) {
            check(reply->blob->digest == "deadbeef", "blob carries the sink's digest");
            check(reply->blob->size == 50, "blob size matches the full body, not the preview");
        }
    }
    check(sunk_bytes == full_body, "the sink receives the FULL body, not the truncated preview");
}

void test_over_threshold_without_blob_sink() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::NetOut{{"example.com:80:http"}, std::nullopt, {}}});
    auto ctx = make_ctx(held);
    ctx.tool_result_byte_threshold = 5;  // no blob_sink wired at all

    FakeEgressBackend backend;
    backend.response = sandbox::NetEgressResponse{200, {}, std::string(50, 'y')};

    auto reply = ReadContent::invoke_via(ReadContent::Args{.url = "http://example.com/big", .path = std::nullopt}, ctx, backend);
    check(reply.has_value(), "over-threshold with no sink still succeeds -- a bounded preview, not a hard failure");
    if (reply) {
        check(reply->truncated, "still flagged truncated");
        check(reply->preview.size() == 5, "preview is still bounded even with no sink to hand off the rest");
        check(!reply->blob.has_value(), "no blob possible with no sink -- but no error either");
    }
}

void test_capability_not_held_never_calls_backend() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held;  // empty -- no NetOut grant at all
    auto ctx = make_ctx(held);

    FakeEgressBackend backend;
    auto reply = ReadContent::invoke_via(ReadContent::Args{.url = "http://example.com/data", .path = std::nullopt}, ctx, backend);
    check(!reply.has_value(), "no NetOut grant -> denied");
    if (!reply) check(reply.error().code == "tool.capability_not_held", "denial uses the standard capability error code");
    check(backend.fetch_calls == 0, "the backend is never touched when the capability check fails (I2)");
}

void test_grant_for_different_host_is_not_used() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
        {agentengine::cap::NetOut{{"other.example.com:80:http"}, std::nullopt, {}}});
    auto ctx = make_ctx(held);

    FakeEgressBackend backend;
    auto reply = ReadContent::invoke_via(ReadContent::Args{.url = "http://example.com/data", .path = std::nullopt}, ctx, backend);
    check(!reply.has_value(), "a NetOut grant for a DIFFERENT host does not cover this request");
    check(backend.fetch_calls == 0, "backend never touched -- no capability laundering across hosts");
}

void test_http_error_status_is_mapped() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::NetOut{{"example.com:80:http"}, std::nullopt, {}}});
    auto ctx = make_ctx(held);

    FakeEgressBackend backend;
    backend.response = sandbox::NetEgressResponse{404, {}, "not found"};

    auto reply = ReadContent::invoke_via(ReadContent::Args{.url = "http://example.com/missing", .path = std::nullopt}, ctx, backend);
    check(!reply.has_value(), "a non-2xx status is a tool error, not a success with a body");
    if (!reply) check(reply.error().code == "read_content.http_error", "http error uses its own error code");
}

void test_malformed_url_never_calls_backend() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::NetOut{{"example.com:80:http"}, std::nullopt, {}}});
    auto ctx = make_ctx(held);

    FakeEgressBackend backend;
    auto reply = ReadContent::invoke_via(ReadContent::Args{.url = "not-a-url", .path = std::nullopt}, ctx, backend);
    check(!reply.has_value(), "a malformed url is rejected before any capability check or fetch");
    check(backend.fetch_calls == 0, "backend never touched for a malformed url");
}

// ---- Track B: the `path` source -----------------------------------------------------------------

void test_ambiguous_source_neither_set() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held;
    auto ctx = make_ctx(held);

    ReadContent::Args args;  // both url and path left unset
    auto reply = ReadContent::invoke(args, ctx);
    check(!reply.has_value(), "neither url nor path set -> rejected");
    if (!reply) check(reply.error().code == "read_content.ambiguous_source", "ambiguity uses its own error code");
}

void test_ambiguous_source_both_set() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held;
    auto ctx = make_ctx(held);

    ReadContent::Args args;
    args.url = "http://example.com/data";
    args.path = "foo.txt";
    auto reply = ReadContent::invoke(args, ctx);
    check(!reply.has_value(), "both url and path set -> rejected");
    if (!reply) check(reply.error().code == "read_content.ambiguous_source", "ambiguity uses its own error code");
}

void test_path_source_no_sandbox() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held;
    auto ctx = make_ctx(held);  // ctx.sandbox_fs left at its default nullptr

    ReadContent::Args args;
    args.path = "foo.txt";
    auto reply = ReadContent::invoke_from_sandbox(args, ctx);
    check(!reply.has_value(), "no sandbox_fs wired -> the tool fails, not a crash or silent no-op");
    if (!reply) check(reply.error().code == "read_content.no_sandbox", "the failure names exactly why");
}

void test_path_source_capability_denied_never_touches_adapter() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held;  // no FsRead grant at all
    auto ctx = make_ctx(held);
    FakeFileSystemAdapter adapter;
    ctx.sandbox_fs = &adapter;

    ReadContent::Args args;
    args.path = "secret.txt";
    auto reply = ReadContent::invoke_from_sandbox(args, ctx);
    check(!reply.has_value(), "no FsRead grant -> denied");
    if (!reply) check(reply.error().code == "tool.capability_not_held", "denial uses the standard capability error code");
    check(adapter.read_file_calls == 0, "the adapter is never touched when the capability check fails (I2)");
}

void test_path_source_grant_for_different_prefix_is_not_used() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::FsRead{"work", "other-dir", std::nullopt}});
    auto ctx = make_ctx(held);
    FakeFileSystemAdapter adapter;
    ctx.sandbox_fs = &adapter;

    ReadContent::Args args;
    args.path = "secret.txt";
    auto reply = ReadContent::invoke_from_sandbox(args, ctx);
    check(!reply.has_value(), "a FsRead grant for a DIFFERENT prefix does not cover this path");
    check(adapter.read_file_calls == 0, "adapter never touched -- no capability laundering across paths");
}

void test_path_source_happy_path_under_threshold() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::FsRead{"work", "", std::nullopt}});
    auto ctx = make_ctx(held);
    FakeFileSystemAdapter adapter;
    adapter.response = to_bytes("hello from the sandbox mount");
    ctx.sandbox_fs = &adapter;

    ReadContent::Args args;
    args.path = "hello.txt";
    auto reply = ReadContent::invoke_from_sandbox(args, ctx);
    check(reply.has_value(), "reading a real (faked) file through the seam succeeds");
    if (reply) {
        check(reply->preview == "hello from the sandbox mount", "preview is the whole body under threshold");
        check(!reply->truncated, "not truncated under threshold");
        check(reply->media_type == "application/octet-stream", "no Content-Type source -- generic default");
    }
    check(adapter.read_file_calls == 1, "adapter.read_file called exactly once");
    if (adapter.last_path) check(*adapter.last_path == "hello.txt", "the requested path reaches the adapter verbatim");
}

void test_path_source_over_threshold_with_blob_sink() {
    using agentengine::tools::ReadContent;
    agentengine::CapabilitySet const held =
        agentengine::CapabilitySet::grant_root({agentengine::cap::FsRead{"work", "", std::nullopt}});
    auto ctx = make_ctx(held);
    ctx.tool_result_byte_threshold = 5;
    FakeFileSystemAdapter adapter;
    std::string const full_body(50, 'z');
    adapter.response = to_bytes(full_body);
    ctx.sandbox_fs = &adapter;

    std::string sunk_bytes;
    ctx.blob_sink = [&](std::span<std::byte const> bytes, std::string const&) -> agentengine::result<agentengine::BlobRef> {
        sunk_bytes.assign(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        return agentengine::BlobRef{"deadbeef", "application/octet-stream", bytes.size(), "test-store"};
    };

    ReadContent::Args args;
    args.path = "big.bin";
    auto reply = ReadContent::invoke_from_sandbox(args, ctx);
    check(reply.has_value(), "over-threshold path read still succeeds when a sink is wired");
    if (reply) {
        check(reply->truncated, "flagged truncated over threshold");
        check(reply->preview.size() == 5, "preview is bounded to the threshold, not the whole file");
        check(reply->total_bytes == 50, "total_bytes reports the REAL size, not the preview's");
        check(reply->blob.has_value(), "a blob is promoted over threshold when a sink exists");
    }
    check(sunk_bytes == full_body, "the sink receives the FULL file content, not the truncated preview -- "
                                    "the same build_reply path the url source already proved");
}

}  // namespace

int main() {
    test_url_parsing();
    test_happy_path_under_threshold();
    test_over_threshold_with_blob_sink();
    test_over_threshold_without_blob_sink();
    test_capability_not_held_never_calls_backend();
    test_grant_for_different_host_is_not_used();
    test_http_error_status_is_mapped();
    test_malformed_url_never_calls_backend();

    test_ambiguous_source_neither_set();
    test_ambiguous_source_both_set();
    test_path_source_no_sandbox();
    test_path_source_capability_denied_never_touches_adapter();
    test_path_source_grant_for_different_prefix_is_not_used();
    test_path_source_happy_path_under_threshold();
    test_path_source_over_threshold_with_blob_sink();

    if (g_failures == 0) {
        std::printf("test_read_content: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_read_content: %d check(s) failed\n", g_failures);
    return 1;
}
