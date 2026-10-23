// devsim/verify-preamble — verifies the preamble now shown at the top of api/pizza-order.html.
// The preamble (includes + user_message + text_message + drive) is transcribed below exactly as the
// page shows it; this file adds only a trivial task to drive and a main(), to prove the head of the
// file is real: correct includes, and drive() implemented over the engine's own rt::block_on.
#include <cstdio>
#include <memory_resource>
#include <string>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

// text_message() is the SAME helper under the name the workflow examples give it
// (examples/23_handoff_mesh.cpp:52) -- two spellings of one shape, because two files wrote it.
[[nodiscard]] Message text_message(std::string text) { return user_message(std::move(text)); }

// Every act below calls drive(...) to run a task to completion synchronously. The repo's examples
// define it locally as a resume loop (examples/05:150-156); the engine's OWN equivalent, and the one
// to use in your application, is agentengine::rt::block_on(task) -- correct under genuine
// cross-thread contention, and it refuses rather than deadlocks on a lane or offload worker.
template <class T>
T drive(agentengine::rt::task<T> t) {
    return agentengine::rt::block_on(std::move(t));
}

// ---- test-only: something for drive() to drive -------------------------------------------------
namespace {

// A nested task is lazily started, so awaiting it genuinely suspends the outer frame -- which is
// what drive()/block_on has to handle.
agentengine::rt::task<int> inner() { co_return 3; }

agentengine::rt::task<int> outer() {
    int const v = co_await inner();
    co_return v;
}

}  // namespace

int main() {
    int g_failures = 0;

    // user_message / text_message build the same Message shape.
    Message const a = user_message("hello");
    Message const b = text_message("hello");
    if (a.role != role::user || b.role != role::user) { std::fprintf(stderr, "FAIL: role\n"); ++g_failures; }
    if (text_of(a) != "hello" || text_of(b) != "hello") {
        std::fprintf(stderr, "FAIL: text_of round-trip: '%s' / '%s'\n", text_of(a).c_str(), text_of(b).c_str());
        ++g_failures;
    } else {
        std::printf("  ok: user_message/text_message round-trip through text_of\n");
    }

    // drive() over the engine's block_on drives a genuinely suspending task to completion.
    int const total = drive(outer());
    if (total != 3) { std::fprintf(stderr, "FAIL: drive returned %d, expected 3\n", total); ++g_failures; }
    else { std::printf("  ok: drive() == rt::block_on drives a suspending task to completion\n"); }

    std::fprintf(stderr, g_failures == 0 ? "verify_preamble: OK\n" : "verify_preamble: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
