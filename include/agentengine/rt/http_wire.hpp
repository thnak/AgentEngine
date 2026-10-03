#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §6.3 ("HTTP/1.1 + SSE: keep the in-house
// client (net_egress_proxy.cpp, sse_stream_pump.hpp), rewritten as coroutines") -- the I/O-free half of that
// rewrite: building an HTTP/1.1 request, parsing a response head, decoding a response body's framing, and decoding
// server-sent events. Every type here is a pure function of (state, bytes in) -> (bytes / events / verdict out),
// with no I/O and no thread, so each one is tested byte-by-byte with no server (the same discipline as ADR-019's
// sandbox/incremental_http_body.hpp, which these supersede for the async client). rt/http.hpp drives them on the
// reactor.
//
// STRICTNESS (the request-smuggling class, RFC 9112 §6.3 / §11.2, and ADR-011 claim C9). The blocking client is
// lenient: it takes the first Content-Length, treats any Transfer-Encoding containing "chunked" as chunked, and
// accepts any line ending. This parser is not, because a response two parsers disagree about is how smuggling and
// response-splitting happen:
//   - the head must use CRLF line endings; a bare CR or bare LF, or a NUL, is `malformed_response`;
//   - obsolete line folding (a header line starting with SP/HTAB) is REJECTED (`net.obs_fold_rejected`), never
//     unfolded (RFC 9112 §5.2 permits rejecting it);
//   - whitespace between a field name and its colon is rejected (RFC 9112 §5.1: MUST reject);
//   - Content-Length AND Transfer-Encoding together are rejected (`ambiguous_framing`, RFC 9112 §6.3 rule 3);
//   - several Content-Length values (repeated fields or a comma list) must all be the same number, else
//     `ambiguous_framing`; a value that is not 1*DIGIT is `malformed_response`;
//   - Transfer-Encoding must be exactly `chunked` (one coding, case-insensitive); anything else -- gzip, chunked
//     twice, chunked not last -- is `ambiguous_framing` (`net.transfer_coding_unsupported`): this client asks for
//     no transfer coding and must not guess where a body it cannot decode ends;
//   - every size is capped DURING parsing, never after buffering: head bytes and field count
//     (`header_too_large`), body wire bytes (`body_too_large`, framing included -- the blocking client also caps
//     raw bytes), a chunk-size line (1 KiB, as `ChunkedBodyDecoder`), trailer bytes (counted against the head cap).
// Request building rejects at build time, before any connection exists: a CR, LF or NUL anywhere (method, target,
// host, header name or value -- ADR-011 C9's `reject_crlf`, applied to every field), a method that is not an RFC
// 9110 token, a target that is not origin-form (`/...`) or contains whitespace or a control character, a header
// name that is not a token, and a caller-supplied Host, Content-Length, Transfer-Encoding or Connection header --
// framing and connection management are this client's, so a caller cannot desynchronise them from the body.
//
// SSE (the WHATWG "event stream interpretation", html.spec.whatwg.org §9.2.6), incremental across arbitrary chunk
// boundaries: a UTF-8 BOM at the very start is dropped; lines end in CRLF, LF or CR (a CRLF split between two
// feeds is one line end); `event`, `data` (several data lines are joined with "\n"), `id` (sticky; a value with a
// NUL is ignored), `retry` (ASCII digits only, else ignored); lines starting with ':' are comments; unknown fields
// are ignored; a field with no colon has an empty value; ONE leading space of a value is removed; a block whose
// data buffer is empty dispatches nothing; an event left incomplete at end of stream is NOT dispatched (reported
// through `incomplete()`). Bytes are not UTF-8-validated (no U+FFFD replacement): the consumers parse JSON and
// do their own validation. Every event is capped (`max_event_bytes`, line buffer + data + type + id), so a peer
// that never sends a newline cannot grow memory without bound.
// Differences from the existing provider path (sandbox::SseEventFramer + split_sse_named_events /
// split_sse_data_events), which is a block splitter plus per-vendor line scans rather than an SSE parser: it
// splits blocks only on "\n\n" / "\r\n\r\n" (not a bare CR), emits ONE event per data line instead of joining
// them, strips ALL leading spaces of a value instead of one, and has no BOM, id or retry handling. On streams of
// single-data-line events with consistent LF or CRLF endings -- every real OpenAI/Anthropic stream -- the two
// produce identical events; tests/rt/test_rt_http*.cpp proves that differentially.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentengine::rt {

// What went wrong with an HTTP exchange. Values, never exceptions (001 §6). `code` strings on the results reuse the
// blocking client's stable `net.*` codes where one exists (net_egress_proxy.cpp), so the switch-over keeps them.
// ae-naming-lint: allow http_error — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class http_error : std::uint8_t {
    none,                // success
    canceled,            // the caller's stop token (or the reactor / DNS pool shutting down)
    deadline_exceeded,   // the overall request deadline (`HttpOptions::total_timeout`) passed
    idle_timeout,        // no progress on one connect/handshake/write/read within `HttpOptions::idle_timeout`
    consumer_stalled,    // the consumer did not ask for the next chunk/event within the stall bound; stream closed
    invalid_request,     // rejected while building the request (CRLF/NUL injection, bad method/target/header)
    no_policy,           // no AddressPolicy: refused before any lookup (fail closed, I2)
    resolve_failed,      // the host did not resolve
    address_rejected,    // the AddressPolicy accepted none of the resolved addresses; nothing was connected
    connect_failed,      // every accepted address refused / was unreachable
    tls_unavailable,     // https requested but no TLS connector was given (or the build has none)
    tls_failed,          // handshake or certificate failure; `code` is the TLS client's (net.tls_*)
    transport,           // a read or write failed after connecting
    malformed_response,  // the status line, a header line or the chunked framing is not valid HTTP/1.1
    ambiguous_framing,   // Content-Length + Transfer-Encoding, conflicting Content-Lengths, an unsupported coding
    header_too_large,    // the head exceeded `max_header_bytes` or `max_header_count`
    body_too_large,      // the body exceeded the byte cap (a `resource` error, as net.byte_cap_exceeded)
    event_too_large,     // one SSE event exceeded `max_event_bytes` (a `resource` error)
    truncated,           // the connection ended before the head, the declared length or the final chunk
    busy,                // a second next() while one is outstanding on the same reader (nothing was touched)
};

// True for the errors the engine classifies `resource` (§6.3 / §9 D6, as net.byte_cap_exceeded is today).
[[nodiscard]] constexpr bool is_resource_error(http_error e) noexcept {
    return e == http_error::body_too_large || e == http_error::header_too_large ||
           e == http_error::event_too_large || e == http_error::consumer_stalled;
}

// ae-naming-lint: allow HttpHeaders — ADR-237 §6.3: the blocking client's own header list shape (NetEgressResponse)
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// The bound every response is held to. Defaults match the blocking client where it has one.
// ae-naming-lint: allow HttpLimits — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct HttpLimits {
    // The blocking client's hard ceiling (sandbox::kHardResponseCeilingBytes, 16 MiB); `max_body_bytes` can only
    // narrow it, never widen it (020 §1). Kept as its own constant: rt/ is L0 and may not include sandbox/ (L1).
    static constexpr std::uint64_t kHardBodyCeilingBytes = 16u * 1024u * 1024u;

    std::size_t   max_header_bytes = 64u * 1024u;  // status line + fields + the blank line; trailers count too
    std::size_t   max_header_count = 100;
    std::uint64_t max_body_bytes   = kHardBodyCeilingBytes;  // wire bytes of the body, framing included

    [[nodiscard]] std::uint64_t effective_body_cap() const noexcept {
        return std::min<std::uint64_t>(max_body_bytes, kHardBodyCeilingBytes);
    }
};

namespace http_wire {

// ---- verdicts ---------------------------------------------------------------------------------------------

// ae-naming-lint: allow WireError — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct WireError {
    http_error  error = http_error::none;
    std::string code;     // stable net.* code
    std::string message;  // human-readable detail

    [[nodiscard]] bool ok() const noexcept { return error == http_error::none; }
};

[[nodiscard]] inline WireError fail(http_error e, std::string code, std::string message) {
    return WireError{e, std::move(code), std::move(message)};
}

// ---- character classes (RFC 9110 §5.6.2, §5.5) ------------------------------------------------------------

[[nodiscard]] constexpr bool is_tchar(char c) noexcept {
    if (c >= '0' && c <= '9') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-': case '.':
        case '^': case '_': case '`': case '|': case '~': return true;
        default: return false;
    }
}

[[nodiscard]] constexpr bool is_token(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) {
        if (!is_tchar(c)) return false;
    }
    return true;
}

// field-vchar / SP / HTAB / obs-text: everything but the control characters (and DEL), HTAB allowed.
[[nodiscard]] constexpr bool is_field_value_char(char c) noexcept {
    auto const u = static_cast<unsigned char>(c);
    return u == '\t' || (u >= 0x20 && u != 0x7F);
}

[[nodiscard]] constexpr char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool equals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    }
    return true;
}

[[nodiscard]] constexpr std::string_view trim_ows(std::string_view v) noexcept {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
}

// ---- request building -------------------------------------------------------------------------------------

// ADR-011 claim C9 for every field: a CR, LF or NUL is the request-splitting / header-injection primitive.
[[nodiscard]] inline WireError reject_injection(std::string_view field, std::string_view value) {
    if (value.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos) {
        return fail(http_error::invalid_request, "net.header_injection_rejected",
                    "value for '" + std::string(field) + "' contains a CR, LF or NUL byte");
    }
    return {};
}

// The fields a request is built from. Host and port are the connection's; `target` is origin-form ("/path?q").
// ae-naming-lint: allow RequestLine — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct RequestLine {
    std::string_view   method;
    std::string_view   target;
    std::string_view   host;
    std::uint16_t      port         = 0;
    std::uint16_t      default_port = 80;  // 80 for http, 443 for https: omitted from Host when equal
    HttpHeaders const* headers      = nullptr;
    std::string_view   body;
};

// Builds the raw request (or reports why it may not be sent). Always `Connection: close` -- one exchange per
// connection, as the blocking client (keep-alive pooling is a later performance step, §6.3). A body is framed by
// Content-Length (never chunked); POST/PUT/PATCH with no body send `Content-Length: 0`.
[[nodiscard]] inline WireError build_request(RequestLine const& r, std::string* out) {
    if (auto e = reject_injection("method", r.method); !e.ok()) return e;
    if (auto e = reject_injection("target", r.target); !e.ok()) return e;
    if (auto e = reject_injection("host", r.host); !e.ok()) return e;
    if (!is_token(r.method)) {
        return fail(http_error::invalid_request, "net.invalid_request", "the method is not an HTTP token");
    }
    if (r.target.empty() || r.target.front() != '/') {
        return fail(http_error::invalid_request, "net.invalid_request", "the request target must be origin-form ('/...')");
    }
    for (char c : r.target) {
        auto const u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F) {
            return fail(http_error::invalid_request, "net.header_injection_rejected",
                        "the request target contains whitespace or a control character");
        }
    }
    if (r.host.empty()) return fail(http_error::invalid_request, "net.invalid_request", "the host is empty");
    for (char c : r.host) {
        auto const u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F || c == '/' || c == '@' || c == '[' || c == ']') {
            return fail(http_error::invalid_request, "net.invalid_request",
                        "the host contains a character a host name or address may not");
        }
    }
    if (r.headers != nullptr) {
        for (auto const& [name, value] : *r.headers) {
            if (auto e = reject_injection("header name", name); !e.ok()) return e;
            if (auto e = reject_injection("header value", value); !e.ok()) return e;
            if (!is_token(name)) {
                return fail(http_error::invalid_request, "net.invalid_request",
                            "header name '" + name + "' is not an HTTP token");
            }
            for (char c : value) {
                if (!is_field_value_char(c)) {
                    return fail(http_error::invalid_request, "net.header_injection_rejected",
                                "header value for '" + name + "' contains a control character");
                }
            }
            if (equals_ci(name, "host") || equals_ci(name, "content-length") ||
                equals_ci(name, "transfer-encoding") || equals_ci(name, "connection")) {
                return fail(http_error::invalid_request, "net.invalid_request",
                            "header '" + name + "' is managed by the client and may not be supplied");
            }
        }
    }

    std::string req;
    req.reserve(128 + r.body.size());
    req.append(r.method).append(" ").append(r.target).append(" HTTP/1.1\r\nHost: ");
    bool const v6 = r.host.find(':') != std::string_view::npos;  // a numeric IPv6 address: bracketed in Host
    if (v6) req.push_back('[');
    req.append(r.host);
    if (v6) req.push_back(']');
    if (r.port != 0 && r.port != r.default_port) req.append(":").append(std::to_string(r.port));
    req.append("\r\n");
    if (r.headers != nullptr) {
        for (auto const& [name, value] : *r.headers) req.append(name).append(": ").append(value).append("\r\n");
    }
    bool const body_method = equals_ci(r.method, "POST") || equals_ci(r.method, "PUT") || equals_ci(r.method, "PATCH");
    if (!r.body.empty() || body_method) req.append("Content-Length: ").append(std::to_string(r.body.size())).append("\r\n");
    req.append("Connection: close\r\n\r\n");
    req.append(r.body);
    *out = std::move(req);
    return {};
}

// ---- response head ----------------------------------------------------------------------------------------

// ae-naming-lint: allow ResponseHead — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct ResponseHead {
    int           version_minor = 1;  // HTTP/1.<minor>
    std::uint16_t status        = 0;
    std::string   reason;
    HttpHeaders   headers;  // names as received, values with OWS trimmed
};

// Incremental head parser: feed bytes until `done()`; whatever followed the head is in `leftover()`.
// ae-naming-lint: allow HeadParser — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class HeadParser {
public:
    explicit HeadParser(HttpLimits limits = {}) : limits_(limits) {}

    // Appends `bytes`; returns an error (sticky) or ok. `done()` once the blank line arrived and the head parsed.
    [[nodiscard]] WireError feed(std::string_view bytes) {
        if (!error_.ok() || done_) return error_;
        std::size_t const from = buf_.size() >= 3 ? buf_.size() - 3 : 0;
        buf_.append(bytes);
        std::size_t const end = buf_.find("\r\n\r\n", from);
        if (end == std::string::npos) {
            if (buf_.size() > limits_.max_header_bytes) {
                error_ = fail(http_error::header_too_large, "net.header_too_large",
                              "the response head exceeded " + std::to_string(limits_.max_header_bytes) + " bytes");
            }
            return error_;
        }
        if (end + 4 > limits_.max_header_bytes) {
            error_ = fail(http_error::header_too_large, "net.header_too_large",
                          "the response head exceeded " + std::to_string(limits_.max_header_bytes) + " bytes");
            return error_;
        }
        leftover_ = buf_.substr(end + 4);
        buf_.resize(end + 2);  // keep the last line's CRLF: every line is CRLF-terminated
        error_ = parse(buf_);
        buf_.clear();
        buf_.shrink_to_fit();
        if (error_.ok()) done_ = true;
        return error_;
    }

    [[nodiscard]] bool                done() const noexcept { return done_; }
    [[nodiscard]] ResponseHead&       head() noexcept { return head_; }
    [[nodiscard]] std::string&        leftover() noexcept { return leftover_; }
    [[nodiscard]] std::size_t         buffered() const noexcept { return buf_.size(); }

private:
    [[nodiscard]] WireError parse(std::string_view text) {
        // Line endings: every LF must follow a CR and every CR must precede an LF; no NUL anywhere.
        for (std::size_t i = 0; i < text.size(); ++i) {
            char const c = text[i];
            if (c == '\0') return fail(http_error::malformed_response, "net.protocol_error", "NUL in the response head");
            if (c == '\n' && (i == 0 || text[i - 1] != '\r')) {
                return fail(http_error::malformed_response, "net.protocol_error", "bare LF in the response head");
            }
            if (c == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n')) {
                return fail(http_error::malformed_response, "net.protocol_error", "bare CR in the response head");
            }
        }
        std::size_t pos      = 0;
        std::size_t line_end = text.find("\r\n");
        if (auto e = parse_status_line(text.substr(0, line_end)); !e.ok()) return e;
        pos = line_end + 2;
        while (pos < text.size()) {
            line_end              = text.find("\r\n", pos);
            std::string_view line = text.substr(pos, line_end - pos);
            pos                   = line_end + 2;
            if (line.front() == ' ' || line.front() == '\t') {
                return fail(http_error::malformed_response, "net.obs_fold_rejected",
                            "obsolete line folding in a response header (RFC 9112 §5.2)");
            }
            auto const colon = line.find(':');
            if (colon == std::string_view::npos) {
                return fail(http_error::malformed_response, "net.protocol_error", "a header line has no colon");
            }
            std::string_view const name = line.substr(0, colon);
            if (!is_token(name)) {
                return fail(http_error::malformed_response, "net.protocol_error",
                            "a header name is not a token (or has whitespace before its colon)");
            }
            std::string_view const value = trim_ows(line.substr(colon + 1));
            for (char c : value) {
                if (!is_field_value_char(c)) {
                    return fail(http_error::malformed_response, "net.protocol_error",
                                "a header value contains a control character");
                }
            }
            if (head_.headers.size() >= limits_.max_header_count) {
                return fail(http_error::header_too_large, "net.header_too_large",
                            "the response has more than " + std::to_string(limits_.max_header_count) + " header fields");
            }
            head_.headers.emplace_back(std::string(name), std::string(value));
        }
        return {};
    }

    [[nodiscard]] WireError parse_status_line(std::string_view line) {
        auto const bad = [] {
            return fail(http_error::malformed_response, "net.protocol_error", "malformed HTTP status line");
        };
        // HTTP-version SP 3DIGIT [ SP reason-phrase ]
        if (line.size() < 12 || line.substr(0, 7) != "HTTP/1." || (line[7] != '0' && line[7] != '1') ||
            line[8] != ' ') {
            return bad();
        }
        head_.version_minor = line[7] - '0';
        unsigned status     = 0;
        for (std::size_t i = 9; i < 12; ++i) {
            if (line[i] < '0' || line[i] > '9') return bad();
            status = status * 10 + static_cast<unsigned>(line[i] - '0');
        }
        if (status < 100) return bad();
        head_.status = static_cast<std::uint16_t>(status);
        if (line.size() > 12) {
            if (line[12] != ' ') return bad();
            std::string_view const reason = line.substr(13);
            for (char c : reason) {
                if (!is_field_value_char(c)) return bad();
            }
            head_.reason = std::string(reason);
        }
        return {};
    }

    HttpLimits   limits_;
    std::string  buf_;
    std::string  leftover_;
    ResponseHead head_;
    WireError    error_;
    bool         done_ = false;
};

// ---- body framing (RFC 9112 §6.3) -------------------------------------------------------------------------

// ae-naming-lint: allow body_framing — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
enum class body_framing : std::uint8_t {
    none,         // HEAD, 1xx, 204, 304: no body whatever the fields say
    length,       // Content-Length
    chunked,      // Transfer-Encoding: chunked
    until_close,  // neither: the body ends when the server closes (Connection: close was sent)
};

// ae-naming-lint: allow Framing — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct Framing {
    body_framing  kind   = body_framing::until_close;
    std::uint64_t length = 0;
};

[[nodiscard]] inline WireError determine_framing(std::string_view method, ResponseHead const& head,
                                                 HttpLimits const& limits, Framing* out) {
    bool                     has_te = false;
    std::vector<std::string> te_codings;
    bool                     has_cl = false;
    std::optional<std::uint64_t> cl;
    for (auto const& [name, value] : head.headers) {
        if (equals_ci(name, "transfer-encoding")) {
            has_te = true;
            std::string_view rest = value;
            while (true) {
                auto const comma = rest.find(',');
                std::string_view const item = trim_ows(rest.substr(0, comma));
                if (!item.empty()) {
                    std::string lowered(item);
                    for (char& c : lowered) c = lower_ascii(c);
                    te_codings.push_back(std::move(lowered));
                }
                if (comma == std::string_view::npos) break;
                rest.remove_prefix(comma + 1);
            }
        } else if (equals_ci(name, "content-length")) {
            has_cl = true;
            std::string_view rest = value;
            while (true) {
                auto const comma = rest.find(',');
                std::string_view const item = trim_ows(rest.substr(0, comma));
                if (item.empty() || item.size() > 19 ||
                    !std::all_of(item.begin(), item.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                    return fail(http_error::malformed_response, "net.content_length_invalid",
                                "Content-Length is not a decimal number");
                }
                std::uint64_t n = 0;
                for (char c : item) n = n * 10 + static_cast<std::uint64_t>(c - '0');
                if (cl.has_value() && *cl != n) {
                    return fail(http_error::ambiguous_framing, "net.content_length_conflict",
                                "the response carries conflicting Content-Length values");
                }
                cl = n;
                if (comma == std::string_view::npos) break;
                rest.remove_prefix(comma + 1);
            }
        }
    }
    if (has_te && has_cl) {
        return fail(http_error::ambiguous_framing, "net.framing_conflict",
                    "the response carries both Content-Length and Transfer-Encoding (RFC 9112 §6.3)");
    }
    bool const no_body = equals_ci(method, "HEAD") || (head.status >= 100 && head.status < 200) ||
                         head.status == 204 || head.status == 304;
    if (has_te && !(te_codings.size() == 1 && te_codings.front() == "chunked")) {
        return fail(http_error::ambiguous_framing, "net.transfer_coding_unsupported",
                    "Transfer-Encoding must be exactly 'chunked'");
    }
    if (no_body) {
        *out = Framing{body_framing::none, 0};
        return {};
    }
    if (has_te) {
        *out = Framing{body_framing::chunked, 0};
        return {};
    }
    if (cl.has_value()) {
        if (*cl > limits.effective_body_cap()) {
            return fail(http_error::body_too_large, "net.byte_cap_exceeded",
                        "the declared Content-Length exceeds the byte cap");
        }
        *out = Framing{body_framing::length, *cl};
        return {};
    }
    *out = Framing{body_framing::until_close, 0};
    return {};
}

// Decodes a body's framing incrementally. `feed` consumes wire bytes and appends payload to `out`; once
// `complete()`, further bytes are ignored (Connection: close -- nothing else may follow on this connection).
// ae-naming-lint: allow BodyDecoder — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class BodyDecoder {
public:
    BodyDecoder() = default;
    BodyDecoder(Framing framing, HttpLimits limits)
        : framing_(framing), limits_(limits),
          complete_(framing.kind == body_framing::none ||
                    (framing.kind == body_framing::length && framing.length == 0)),
          remaining_(framing.kind == body_framing::length ? framing.length : 0) {}

    [[nodiscard]] WireError feed(std::string_view in, std::string* out) {
        if (!error_.ok() || complete_ || in.empty()) return error_;
        switch (framing_.kind) {
            case body_framing::none: return error_;
            case body_framing::length: {
                std::size_t const take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, in.size()));
                out->append(in.substr(0, take));
                remaining_ -= take;
                wire_ += take;
                if (remaining_ == 0) complete_ = true;
                return error_;
            }
            case body_framing::until_close:
                wire_ += in.size();
                if (wire_ > limits_.effective_body_cap()) {
                    error_ = fail(http_error::body_too_large, "net.byte_cap_exceeded", "response exceeded the byte cap");
                    return error_;
                }
                out->append(in);
                return error_;
            case body_framing::chunked: return feed_chunked(in, out);
        }
        return error_;
    }

    // The connection ended. Ok for an until-close body; `truncated` when a length or a final chunk is missing.
    [[nodiscard]] WireError on_eof() {
        if (!error_.ok()) return error_;
        if (complete_ || framing_.kind == body_framing::until_close) {
            complete_ = true;
            return {};
        }
        error_ = fail(http_error::truncated, "net.stream_truncated",
                      framing_.kind == body_framing::length
                          ? "the response body ended before its declared Content-Length: the connection was cut"
                          : "the chunked response ended before its final chunk: the connection was cut");
        return error_;
    }

    [[nodiscard]] bool          complete() const noexcept { return complete_; }
    [[nodiscard]] std::uint64_t wire_bytes() const noexcept { return wire_; }
    [[nodiscard]] Framing const& framing() const noexcept { return framing_; }

private:
    enum class cs : std::uint8_t { size, size_ws, ext, size_lf, data, data_cr, data_lf, trailer, trailer_lf, final_lf };

    static constexpr std::size_t kMaxChunkSizeLine = 1024;  // as sandbox::ChunkedBodyDecoder

    [[nodiscard]] WireError bad(char const* what) {
        error_ = fail(http_error::malformed_response, "net.chunked_malformed", what);
        return error_;
    }

    [[nodiscard]] WireError feed_chunked(std::string_view in, std::string* out) {
        std::size_t i = 0;
        while (i < in.size() && !complete_) {
            if (state_ == cs::data) {
                std::size_t const take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, in.size() - i));
                if (!count(take)) return error_;
                out->append(in.substr(i, take));
                i += take;
                remaining_ -= take;
                if (remaining_ == 0) state_ = cs::data_cr;
                continue;
            }
            char const c = in[i++];
            if (!count(1)) return error_;
            switch (state_) {
                case cs::size: {
                    if (++line_ > kMaxChunkSizeLine) return bad("chunk-size line too long");
                    int digit = -1;
                    if (c >= '0' && c <= '9') digit = c - '0';
                    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                    if (digit >= 0) {
                        if (++digits_ > 16 || remaining_ > (UINT64_MAX >> 4)) return bad("chunk size overflows");
                        remaining_ = (remaining_ << 4) | static_cast<std::uint64_t>(digit);
                        break;
                    }
                    if (digits_ == 0) return bad("malformed chunk size");
                    if (c == '\r') state_ = cs::size_lf;
                    else if (c == ';') state_ = cs::ext;
                    else if (c == ' ' || c == '\t') state_ = cs::size_ws;
                    else return bad("malformed chunk size");
                    break;
                }
                case cs::size_ws:  // BWS before a chunk extension
                    if (++line_ > kMaxChunkSizeLine) return bad("chunk-size line too long");
                    if (c == ';') state_ = cs::ext;
                    else if (c == '\r') state_ = cs::size_lf;
                    else if (c != ' ' && c != '\t') return bad("malformed chunk size");
                    break;
                case cs::ext:  // chunk extensions: ignored, bounded, no control characters
                    if (++line_ > kMaxChunkSizeLine) return bad("chunk-size line too long");
                    if (c == '\r') state_ = cs::size_lf;
                    else if (!is_field_value_char(c)) return bad("control character in a chunk extension");
                    break;
                case cs::size_lf:
                    if (c != '\n') return bad("chunk-size line not terminated by CRLF");
                    line_   = 0;
                    digits_ = 0;
                    if (remaining_ == 0) {
                        state_ = cs::trailer;
                    } else {
                        if (remaining_ > limits_.effective_body_cap()) {
                            error_ = fail(http_error::body_too_large, "net.byte_cap_exceeded",
                                          "a chunk exceeds the byte cap");
                            return error_;
                        }
                        state_ = cs::data;
                    }
                    break;
                case cs::data_cr:
                    if (c != '\r') return bad("missing CRLF after chunk data");
                    state_ = cs::data_lf;
                    break;
                case cs::data_lf:
                    if (c != '\n') return bad("missing CRLF after chunk data");
                    state_ = cs::size;
                    break;
                case cs::trailer:  // trailer fields: discarded (as the blocking client), bounded by the head cap
                    if (++trailer_bytes_ > limits_.max_header_bytes) {
                        error_ = fail(http_error::header_too_large, "net.header_too_large", "trailer section too large");
                        return error_;
                    }
                    if (c == '\r') {
                        state_ = trailer_line_ == 0 ? cs::final_lf : cs::trailer_lf;
                    } else if (c == '\n' || c == '\0') {
                        return bad("bare LF or NUL in the trailer section");
                    } else {
                        if (trailer_line_ == 0 && (c == ' ' || c == '\t')) return bad("obsolete line folding in a trailer");
                        ++trailer_line_;
                    }
                    break;
                case cs::trailer_lf:
                    if (c != '\n') return bad("trailer line not terminated by CRLF");
                    trailer_line_ = 0;
                    state_        = cs::trailer;
                    break;
                case cs::final_lf:
                    if (c != '\n') return bad("chunked body not terminated by CRLF");
                    complete_ = true;
                    break;
                case cs::data: break;  // handled above
            }
        }
        return error_;
    }

    // Wire bytes, framing included, against the byte cap (the blocking client also caps raw bytes).
    [[nodiscard]] bool count(std::size_t n) {
        wire_ += n;
        if (wire_ > limits_.effective_body_cap()) {
            error_ = fail(http_error::body_too_large, "net.byte_cap_exceeded", "response exceeded the byte cap");
            return false;
        }
        return true;
    }

    Framing       framing_{body_framing::none, 0};
    HttpLimits    limits_{};
    WireError     error_{};
    bool          complete_      = true;
    std::uint64_t remaining_     = 0;
    std::uint64_t wire_          = 0;
    cs            state_         = cs::size;
    std::size_t   line_          = 0;
    int           digits_        = 0;
    std::size_t   trailer_bytes_ = 0;
    std::size_t   trailer_line_  = 0;
};

}  // namespace http_wire

// ---- server-sent events ---------------------------------------------------------------------------------------

// One dispatched event. `event` is the `event:` field as sent ("" when none: the WHATWG default type "message").
// `id` is the last event id in effect at dispatch (sticky across events). `retry` is set when THIS event's block
// carried a valid `retry:` field; `SseDecoder::retry_ms()` keeps the latest one even from blocks with no data.
// ae-naming-lint: allow SseEvent — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
struct SseEvent {
    std::string                  event;
    std::string                  data;
    std::string                  id;
    std::optional<std::uint64_t> retry;

    friend bool operator==(SseEvent const&, SseEvent const&) = default;
};

// ae-naming-lint: allow SseDecoder — ADR-237 §6.3: new reactor vocabulary, 027 §4 row added when the ADR is Judged
class SseDecoder {
public:
    static constexpr std::size_t kDefaultMaxEventBytes = 1024u * 1024u;

    explicit SseDecoder(std::size_t max_event_bytes = kDefaultMaxEventBytes) : max_event_(max_event_bytes) {}

    // Feeds bytes (any split); appends every event completed by them to `out`. An event over the cap is
    // `event_too_large`, sticky.
    [[nodiscard]] http_wire::WireError feed(std::string_view bytes, std::vector<SseEvent>* out) {
        if (!error_.ok()) return error_;
        std::size_t i = 0;
        if (bom_state_ < 3) {  // a UTF-8 BOM, possibly split across feeds, only at the very start
            static constexpr unsigned char kBom[3] = {0xEF, 0xBB, 0xBF};
            while (i < bytes.size() && bom_state_ < 3) {
                if (static_cast<unsigned char>(bytes[i]) == kBom[bom_state_]) {
                    ++bom_state_;
                    ++i;
                    continue;
                }
                // Not a BOM after all: the bytes matched so far are content.
                std::string const partial(reinterpret_cast<char const*>(kBom), bom_state_);
                bom_state_ = 3;
                if (auto e = take(partial, out); !e.ok()) return e;
            }
        }
        return take(bytes.substr(i), out);
    }

    // True when bytes of an event that was never terminated are buffered (they are discarded at end of stream).
    [[nodiscard]] bool incomplete() const noexcept {
        return !line_.empty() || !data_.empty() || !type_.empty() || (bom_state_ > 0 && bom_state_ < 3);
    }
    [[nodiscard]] std::optional<std::uint64_t> retry_ms() const noexcept { return retry_; }
    [[nodiscard]] std::string const&           last_event_id() const noexcept { return last_id_; }

private:
    [[nodiscard]] http_wire::WireError take(std::string_view bytes, std::vector<SseEvent>* out) {
        std::size_t i = 0;
        while (i < bytes.size()) {
            char const c = bytes[i];
            if (skip_lf_) {
                skip_lf_ = false;
                if (c == '\n') {
                    ++i;
                    continue;
                }
            }
            if (c == '\r' || c == '\n') {
                skip_lf_ = c == '\r';
                ++i;
                line(out);
                line_.clear();
                continue;
            }
            // Bulk-append up to the next line end.
            std::size_t const end = bytes.find_first_of("\r\n", i);
            std::size_t const n   = (end == std::string_view::npos ? bytes.size() : end) - i;
            if (size() + n > max_event_) {
                error_ = http_wire::fail(http_error::event_too_large, "net.sse_event_too_large",
                                         "a server-sent event exceeded " + std::to_string(max_event_) + " bytes");
                return error_;
            }
            line_.append(bytes.substr(i, n));
            i += n;
        }
        return error_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return line_.size() + data_.size() + type_.size(); }

    void line(std::vector<SseEvent>* out) {
        if (line_.empty()) {
            dispatch(out);
            return;
        }
        if (line_.front() == ':') return;  // comment
        std::string_view const l     = line_;
        auto const             colon = l.find(':');
        std::string_view const field = l.substr(0, colon);
        std::string_view       value = colon == std::string_view::npos ? std::string_view{} : l.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (field == "event") {
            type_.assign(value);
        } else if (field == "data") {
            data_.append(value);
            data_.push_back('\n');
        } else if (field == "id") {
            if (value.find('\0') == std::string_view::npos) last_id_.assign(value);  // sticky, set at once
        } else if (field == "retry") {
            if (!value.empty() && value.size() <= 18 &&
                std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
                std::uint64_t n = 0;
                for (char ch : value) n = n * 10 + static_cast<std::uint64_t>(ch - '0');
                retry_       = n;
                block_retry_ = n;
            }
        }
    }

    void dispatch(std::vector<SseEvent>* out) {
        if (data_.empty()) {
            type_.clear();
            block_retry_.reset();
            return;
        }
        data_.pop_back();  // the trailing "\n"
        out->push_back(SseEvent{std::move(type_), std::move(data_), last_id_, block_retry_});
        type_.clear();
        data_.clear();
        block_retry_.reset();
    }

    std::size_t                  max_event_;
    std::string                  line_;
    std::string                  data_;
    std::string                  type_;
    std::string                  last_id_;
    std::optional<std::uint64_t> retry_;
    std::optional<std::uint64_t> block_retry_;
    bool                         skip_lf_   = false;
    int                          bom_state_ = 0;  // 0..2 bytes of a BOM matched; 3 = decided
    http_wire::WireError         error_;
};

}  // namespace agentengine::rt
