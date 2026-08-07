#pragma once
// Implements the framing half of Milestone 5 Phase C/D2's named gap (ADR-019): decoding an HTTP
// response body AS IT ARRIVES rather than after it is fully buffered.
//
// Two stateful decoders, deliberately separate and each a pure function of (state, bytes-in) ->
// bytes/events-out, with no I/O of their own. That is what makes them testable against a byte-by-byte
// feed with no server involved -- the same discipline the one-shot `decode_chunked_body` /
// `split_sse_data_events` helpers already follow, extended to the incremental case.
//
// The one-shot helpers in `protocol/openai/chat_client.hpp` are NOT replaced: they remain the
// non-streaming `chat()` path's decoder, where the whole body genuinely is in hand at once. These are
// their streaming counterparts, and the backends' own accumulators are written so the one-shot parse
// is expressed in terms of the incremental one -- so the two cannot drift.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine::sandbox {

// RFC 9112 §7.1 chunked transfer-coding, fed incrementally. `feed()` accepts an arbitrary slice of the
// wire (a chunk-size line may be split mid-digit across two calls, a chunk body across many) and
// returns whatever decoded payload bytes became available. Trailers after the terminal 0-size chunk
// are ignored, matching the one-shot decoder this mirrors.
//
// Malformed input is a `contract` failure, never a silent truncation: a decoder that quietly returned
// "" for garbage would present a corrupt stream as an empty-but-successful one.
class ChunkedBodyDecoder {  // ae-naming-lint: allow ChunkedBodyDecoder — new ADR-019 framing vocabulary; 027 has not been updated to list it
public:
    [[nodiscard]] result<std::string> feed(std::string_view bytes) {
        pending_.append(bytes);
        std::string out;
        for (;;) {
            if (done_) return out;
            if (need_ == 0) {
                // Between chunks: consume the CRLF that terminated the previous chunk's data, then a
                // chunk-size line. Both may be incomplete right now, which is not an error -- just
                // "come back with more bytes".
                if (expect_crlf_) {
                    if (pending_.size() < 2) return out;
                    if (pending_.compare(0, 2, "\r\n") != 0) return malformed("missing CRLF after chunk data");
                    pending_.erase(0, 2);
                    expect_crlf_ = false;
                    continue;
                }
                auto const eol = pending_.find("\r\n");
                if (eol == std::string::npos) {
                    // A chunk-size line is short; an unbounded one means the peer is not speaking
                    // chunked at all, and buffering it forever would be a memory DoS.
                    if (pending_.size() > kMaxChunkSizeLine) return malformed("chunk-size line too long");
                    return out;
                }
                std::string_view size_line(pending_.data(), eol);
                if (auto const semi = size_line.find(';'); semi != std::string_view::npos) {
                    size_line = size_line.substr(0, semi);  // chunk extensions, ignored
                }
                std::size_t size = 0;
                if (!parse_hex(size_line, &size)) return malformed("malformed chunk size");
                pending_.erase(0, eol + 2);
                if (size == 0) {
                    done_ = true;
                    return out;
                }
                need_ = size;
                continue;
            }
            if (pending_.empty()) return out;
            std::size_t const take = std::min(need_, pending_.size());
            out.append(pending_, 0, take);
            pending_.erase(0, take);
            need_ -= take;
            if (need_ == 0) expect_crlf_ = true;
        }
    }

    // True once the terminal 0-size chunk has been seen. A stream that ends without one was truncated
    // by the peer -- the caller decides whether that matters (for SSE it usually does not: the events
    // already delivered are still valid).
    [[nodiscard]] bool complete() const noexcept { return done_; }

private:
    static constexpr std::size_t kMaxChunkSizeLine = 1024;

    [[nodiscard]] static result<std::string> malformed(char const* what) {
        return std::unexpected(error{failure_class::contract, what, "net.chunked_malformed"});
    }

    [[nodiscard]] static bool parse_hex(std::string_view text, std::size_t* out) {
        if (text.empty()) return false;
        std::size_t value = 0;
        for (char c : text) {
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A') + 10;
            else if (c == ' ' || c == '\t') continue;  // tolerate padding around the size token
            else return false;
            if (value > (static_cast<std::size_t>(-1) - digit) / 16) return false;  // overflow
            value = value * 16 + digit;
        }
        *out = value;
        return true;
    }

    std::string pending_;
    std::size_t need_ = 0;
    bool expect_crlf_ = false;
    bool done_ = false;
};

// Server-sent events framing, fed incrementally: accumulates bytes and returns each COMPLETE event
// block (everything up to a blank line) as soon as its terminator arrives.
//
// Returns raw blocks rather than parsed fields on purpose. The two backends read different things out
// of a block -- the OpenAI-compatible one only ever needs `data:`, while Anthropic's named-event shape
// needs `event:` too -- so parsing here would mean encoding one vendor's dialect into shared framing.
// Blocks are exactly what both already know how to read.
class SseEventFramer {  // ae-naming-lint: allow SseEventFramer — new ADR-019 framing vocabulary; 027 has not been updated to list it
public:
    [[nodiscard]] std::vector<std::string> feed(std::string_view bytes) {
        pending_.append(bytes);
        std::vector<std::string> blocks;
        for (;;) {
            // Both terminators are live on the wire: real servers send "\n\n", and CRLF-normalising
            // intermediaries produce "\r\n\r\n". Take whichever appears first.
            auto const lf = pending_.find("\n\n");
            auto const crlf = pending_.find("\r\n\r\n");
            std::size_t end = std::string::npos;
            std::size_t skip = 0;
            if (lf != std::string::npos && (crlf == std::string::npos || lf < crlf)) {
                end = lf;
                skip = 2;
            } else if (crlf != std::string::npos) {
                end = crlf;
                skip = 4;
            }
            if (end == std::string::npos) return blocks;
            blocks.emplace_back(pending_, 0, end);
            pending_.erase(0, end + skip);
        }
    }

    // Whatever is left unterminated at end of stream. A well-behaved server ends with a blank line, so
    // this is normally empty -- but a truncated final event is real, and silently dropping it would
    // lose a delivered item.
    [[nodiscard]] std::string take_remainder() {
        std::string out;
        out.swap(pending_);
        return out;
    }

private:
    std::string pending_;
};

}  // namespace agentengine::sandbox
