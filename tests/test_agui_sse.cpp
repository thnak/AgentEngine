// Milestone 7 Phase E3 (013-UI-and-Streaming-Surfaces.md §4, docs/research/2026-a2a-and-agui-detail.md
// §B.3, docs/planning/milestone-7-protocol-conformance-breakdown.md). Proves the SSE framing
// (protocol/agui/sse.hpp) against real `RunEventProjector` output -- a genuine internal event,
// projected to a real AG-UI event, framed exactly as §4 specifies.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/protocol/agui/projection.hpp"
#include "agentengine/protocol/agui/sse.hpp"

namespace {

int  g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

namespace agui = agentengine::agui;
namespace json = agentengine::json;

}  // namespace

int main() {
    // --- E3-1: exact framing shape -- "data: <json>\n\n", nothing more, nothing less --------------
    {
        agui::AgUiEvent event = agui::RunStarted{"thread-1", "run-1", std::nullopt};
        std::string frame = agui::to_sse_frame(event);
        check(frame.rfind("data: ", 0) == 0, "E3-1: the frame begins with the literal \"data: \" prefix");
        check(frame.size() >= 2 && frame.substr(frame.size() - 2) == "\n\n",
              "E3-1: the frame ends with the SSE blank-line terminator \"\\n\\n\"");
        // The bytes between "data: " and the trailing "\n\n" must be exactly one valid JSON value.
        std::string const json_text = frame.substr(6, frame.size() - 6 - 2);
        auto parsed = json::parse(json_text);
        check(parsed.has_value(), "E3-1: the framed payload is valid, parseable JSON");
        if (parsed.has_value()) {
            check(parsed->find("type")->as_string() == "RUN_STARTED",
                  "E3-1: the framed JSON is the real projected event, not a placeholder");
        }
    }

    // --- E3-2: a delta containing an embedded newline stays a SINGLE data: line -- json::dump()'s --
    // --- own string escaping is what makes this safe (sse.hpp's own file-top comment).            ---
    {
        agui::AgUiEvent event = agui::TextMessageContent{"m-1", "line one\nline two"};
        std::string frame = agui::to_sse_frame(event);
        // Exactly one real newline in the whole frame BEFORE the trailing "\n\n" terminator's own
        // first '\n' -- i.e. the embedded "\n" in the delta text must NOT appear as a literal
        // newline byte; it must be the two-character escape sequence \n inside the JSON string.
        check(frame.find("\\n") != std::string::npos,
              "E3-2: the embedded newline is JSON-escaped (\\n, two characters) inside the payload");
        // Count literal newline BYTES in the whole frame -- must be exactly 2 (the trailing "\n\n"),
        // never 3+, which would mean the embedded newline leaked through unescaped.
        std::size_t const total_newlines =
            static_cast<std::size_t>(std::count(frame.begin(), frame.end(), '\n'));
        check(total_newlines == 2,
              "E3-2: the frame has exactly two newline bytes (the trailing terminator) -- the "
              "embedded newline in the delta text never produced a THIRD one");
    }

    // --- E3-3: end to end -- a real projected sequence, framed in order, concatenated ---------------
    {
        agui::RunEventProjector projector("thread-2");
        std::vector<agui::AgUiEvent> events;
        for (auto const& e : projector.project(ae::RunEvent{"run-1", 1, ae::run_event_kind::run_started,
                                                              ae::run_event_payload::Empty{}})) {
            events.push_back(e);
        }
        for (auto const& e : projector.project(
                 ae::RunEvent{"run-1", 2, ae::run_event_kind::run_finished, ae::run_event_payload::Empty{}})) {
            events.push_back(e);
        }
        check(events.size() == 2, "E3-3: two internal events project to two AG-UI events");

        std::string stream = agui::to_sse_stream(events);
        // Exactly two "data: " occurrences, in order (RUN_STARTED before RUN_FINISHED).
        std::size_t first = stream.find("data: ");
        std::size_t second = stream.find("data: ", first + 1);
        check(first != std::string::npos && second != std::string::npos,
              "E3-3: the concatenated stream carries two distinct data: frames");
        if (first != std::string::npos && second != std::string::npos) {
            check(stream.find("RUN_STARTED") < stream.find("RUN_FINISHED"),
                  "E3-3: RUN_STARTED appears before RUN_FINISHED -- order preserved (013 §1: "
                  "\"ordered and monotonic per run\")");
        }
        check(stream == agui::to_sse_frame(events[0]) + agui::to_sse_frame(events[1]),
              "E3-3: to_sse_stream() is exactly the concatenation of individual to_sse_frame() calls");
    }

    if (g_failures == 0) {
        std::printf("test_agui_sse: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_agui_sse: %d failure(s)\n", g_failures);
    return 1;
}
