// Regression proof for GitHub issue #72: both streaming accumulators took a tool-call / content-block
// index straight off the provider's wire, checked only that it was *a number*, and cast it to
// `std::size_t`.
//
// WHY THAT WAS A MEMORY-SAFETY BUG AND NOT A TIDINESS COMPLAINT. Every JSON number is a `double`.
// `static_cast<std::size_t>(-1.0)` is undefined ([conv.fpint]) and on these compilers lands on
// SIZE_MAX. The next line read `if (index >= pending_by_index_.size()) resize(index + 1);` -- which
// looks exactly like a bounds check and is not one, because `SIZE_MAX + 1` wraps to 0, so `resize(0)`
// SUCCEEDS and does nothing. The line after that indexed the vector at SIZE_MAX. Measured before the
// fix: segfault in Release, abort in Debug, on both providers, from one well-formed SSE chunk
// carrying `"index": -1`. A large index instead threw `std::length_error` out of `feed()`, whose
// contract is `result<std::vector<ChatResponseUpdate>>`.
//
// This is inside the threat model these files already state. The Anthropic accumulator's own comment
// hardens against "a malformed or buggy Anthropic-wire-compatible gateway (Bedrock/Vertex/self-hosted
// proxies are a real deployment shape here, not hypothetical)" sending a misrouted delta -- but not
// against that same gateway sending an unbounded index. Its stated stance, that one malformed chunk
// is skipped rather than fatal to the stream, is what this restores.
//
//   H1 (positive control) -- an ordinary two-tool-call stream still accumulates both calls, so every
//         rejection below is measured against a path that demonstrably works.
//   H2 -- OpenAI: a negative index is skipped. The process survives, which before the fix it did not.
//   H3 -- OpenAI: an astronomically large index is skipped, and `feed()` RETURNS rather than throwing
//         `std::length_error` past its own `result<T>` contract.
//   H4 -- OpenAI: a fractional index is skipped. It is not the integer the old cast pretended to read.
//   H5 -- OpenAI: an ABSENT index still means 0. A single-tool-call delta legally omits the field, so
//         this pins the behaviour the fix had to preserve, not just the behaviour it removed.
//   H6 (the design decision, stated as a check) -- a hostile index is DROPPED, never coerced to 0.
//         Clamping to 0 would have been the easy fix and is its own bug: it silently merges an
//         attacker-chosen fragment into the real tool call at index 0.
//   H7 -- Anthropic content_block_start: same three bad indices, same outcome.
//   H8 -- Anthropic content_block_delta: likewise.
//   H9 -- Anthropic content_block_stop: likewise. This site already bounds-checked before indexing so
//         it was never the out-of-bounds write, but the cast itself was still undefined.
//   H10 -- the shared guard `json::as_bounded_integer` in isolation, including the 2^64 boundary that
//         the obvious `d > static_cast<double>(max)` spelling gets wrong.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/json_value.hpp"
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"

#include <cstdio>
#include <limits>
#include <string>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

// One OpenAI tool_calls delta carrying a caller-chosen `index` literal, spelled exactly as it would
// arrive on the wire. `index_literal` is raw JSON text so a test can send things a typed API could
// not express.
[[nodiscard]] std::string openai_stream_with_index(std::string const& index_literal) {
    std::string idx_field = index_literal.empty() ? std::string{} : "\"index\":" + index_literal + ",";
    return "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{" + idx_field +
           "\"id\":\"call-x\",\"function\":{\"name\":\"probe\",\"arguments\":\"{}\"}}]}}]}\n\n"
           "data: [DONE]\n\n";
}

[[nodiscard]] std::string anthropic_start_with_index(std::string const& index_literal) {
    return "event: content_block_start\n"
           "data: {\"type\":\"content_block_start\",\"index\":" +
           index_literal + ",\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
           "event: message_stop\ndata: {}\n\n";
}

[[nodiscard]] std::string anthropic_delta_with_index(std::string const& index_literal) {
    return "event: content_block_delta\n"
           "data: {\"type\":\"content_block_delta\",\"index\":" +
           index_literal + ",\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
           "event: message_stop\ndata: {}\n\n";
}

[[nodiscard]] std::string anthropic_stop_with_index(std::string const& index_literal) {
    return "event: content_block_stop\n"
           "data: {\"type\":\"content_block_stop\",\"index\":" +
           index_literal + "}\n\n"
           "event: message_stop\ndata: {}\n\n";
}

// Feeds a stream and reports whether the parse RETURNED at all. Before the fix the large-index cases
// threw out of a `result<T>`-returning function and the negative-index cases killed the process, so
// "it came back" is itself the claim under test.
template <class ParseFn>
[[nodiscard]] bool survives(ParseFn parse, std::string const& sse) {
    try {
        auto updates = parse(sse);
        return updates.has_value();
    } catch (...) {
        return false;
    }
}

}  // namespace

int main() {
    using agentengine::json::as_bounded_integer;

    auto openai_parse = [](std::string const& sse) {
        return agentengine::openai::detail::parse_streaming_response_into_updates(sse, false);
    };
    auto anthropic_parse = [](std::string const& sse) {
        return agentengine::anthropic::detail::parse_streaming_response_into_updates(sse, false);
    };

    // ---- H1: the positive control. Everything below is a rejection, so first prove acceptance.
    {
        std::string const sse =
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-a\","
            "\"function\":{\"name\":\"alpha\",\"arguments\":\"{}\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call-b\","
            "\"function\":{\"name\":\"beta\",\"arguments\":\"{}\"}}]}}]}\n\n"
            "data: [DONE]\n\n";
        auto updates = openai_parse(sse);
        check(updates.has_value(), "H1 (positive control): an ordinary two-tool-call stream parses");
        check(updates.has_value() && !updates->empty(),
              "H1 (positive control): ... and produces updates, so the guard did not break the "
              "normal path");
    }

    // ---- H2/H3/H4: the three malformed OpenAI indices.
    check(survives(openai_parse, openai_stream_with_index("-1")),
          "H2: OpenAI survives a negative index -- before the fix this segfaulted in Release");
    check(survives(openai_parse, openai_stream_with_index("1e18")),
          "H3: OpenAI returns on an astronomically large index instead of throwing std::length_error "
          "past its own result<T> contract");
    check(survives(openai_parse, openai_stream_with_index("1.5")),
          "H4: OpenAI survives a fractional index");

    // ---- H5: the behaviour the fix had to KEEP. An absent index legally means 0.
    {
        auto updates = openai_parse(openai_stream_with_index(""));
        bool found = false;
        if (updates.has_value()) {
            for (auto const& u : *updates) {
                if (auto const* tc = std::get_if<agentengine::ToolCall>(&u.delta.value)) {
                    if (tc->tool_name == "probe") found = true;
                }
            }
        }
        check(found,
              "H5: a tool_calls delta with NO index field is still accumulated at 0 -- the guard "
              "rejects malformed indices, it does not require the field");
    }

    // ---- H6: the design decision. A hostile index must be dropped, not clamped into index 0.
    {
        std::string const sse =
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-real\","
            "\"function\":{\"name\":\"real_tool\",\"arguments\":\"{}\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":-1,\"id\":\"call-evil\","
            "\"function\":{\"name\":\"_injected\",\"arguments\":\"{}\"}}]}}]}\n\n"
            "data: [DONE]\n\n";
        auto updates = openai_parse(sse);
        bool injected_reached_index_zero = false;
        bool real_call_intact = false;
        if (updates.has_value()) {
            for (auto const& u : *updates) {
                if (auto const* tc = std::get_if<agentengine::ToolCall>(&u.delta.value)) {
                    if (tc->tool_name.find("_injected") != std::string::npos) {
                        injected_reached_index_zero = true;
                    }
                    if (tc->tool_name == "real_tool" && tc->call_id == "call-real") real_call_intact = true;
                }
            }
        }
        check(!injected_reached_index_zero,
              "H6: a negative index is DROPPED, not coerced to 0 -- clamping would merge an "
              "attacker-chosen fragment into the real tool call");
        check(real_call_intact,
              "H6: ... and the genuine call at index 0 is untouched by the rejected fragment");
    }

    // ---- H7/H8/H9: the three Anthropic sites.
    for (auto const& bad : {std::string("-1"), std::string("1e18"), std::string("1.5")}) {
        check(survives(anthropic_parse, anthropic_start_with_index(bad)),
              "H7: Anthropic content_block_start survives index " + bad);
        check(survives(anthropic_parse, anthropic_delta_with_index(bad)),
              "H8: Anthropic content_block_delta survives index " + bad);
        check(survives(anthropic_parse, anthropic_stop_with_index(bad)),
              "H9: Anthropic content_block_stop survives index " + bad);
    }

    // ---- H10: the shared guard on its own.
    {
        using agentengine::json::Value;
        check(as_bounded_integer(Value::make_number(0.0), 4096) == 0u,
              "H10: 0 converts");
        check(as_bounded_integer(Value::make_number(4096.0), 4096) == 4096u,
              "H10: the cap itself converts -- the bound is inclusive");
        check(!as_bounded_integer(Value::make_number(4097.0), 4096).has_value(),
              "H10: one past the cap is refused");
        check(!as_bounded_integer(Value::make_number(-1.0), 4096).has_value(),
              "H10: a negative number is refused, not wrapped to SIZE_MAX");
        check(!as_bounded_integer(Value::make_number(1.5), 4096).has_value(),
              "H10: a fractional number is refused, not truncated");
        check(!as_bounded_integer(Value::make_number(
                      std::numeric_limits<double>::infinity()), 4096).has_value(),
              "H10: infinity is refused");
        check(!as_bounded_integer(Value::make_number(
                      std::numeric_limits<double>::quiet_NaN()), 4096).has_value(),
              "H10: NaN is refused");
        check(!as_bounded_integer(Value::make_string("7"), 4096).has_value(),
              "H10: a non-number is refused rather than coerced");
        // The boundary the obvious spelling gets wrong: `static_cast<double>(UINT64_MAX)` rounds UP
        // to 2^64, so `d > static_cast<double>(max)` would ADMIT exactly 2^64 and then convert it as
        // undefined behaviour. The guard screens on 2^64 as a double before narrowing.
        check(!as_bounded_integer(Value::make_number(18446744073709551616.0)).has_value(),
              "H10: exactly 2^64 is refused even with no explicit cap -- the boundary a naive "
              "comparison against double(UINT64_MAX) would let through");
        check(as_bounded_integer(Value::make_number(4503599627370496.0)).has_value(),
              "H10 (positive control): a large but exactly-representable integer is still accepted, "
              "so the 2^64 screen is a boundary and not a blanket refusal");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
