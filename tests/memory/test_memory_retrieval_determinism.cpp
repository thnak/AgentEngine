// Milestone 4 Phase H3 (docs/planning/milestone-4-sessions-durability-memory-breakdown.md), the
// roadmap's own M4 exit criterion (029 §9 G1): "the default (non-vector) memory retrieval path is
// deterministic and replayable." Phase G4's own test (test_memory_provider.cpp, G4-R7) already
// checked this narrowly -- re-running `on_context()` once and comparing message count plus the
// first message's id. Phase H's own job is to prove the milestone's exit-criterion claim directly
// and more thoroughly: byte-identical `ContextContribution` (every message, every field) across
// MANY repeated calls against a FIXED memory-worktree tree digest and a FIXED turn, with the
// worktree's own tree digest itself provably unchanged by retrieval, and with a tripwire on the
// declared `ChatClient` proving retrieval NEVER makes the network/model call 029 §9 G1 says it
// must not need.

#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

#include "agentengine/core/memory_provider.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "support/run_task_sync.hpp"

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

// Counts every call -- retrieval (on_context()) must NEVER touch this, only on_turn_end() (which
// this test never calls) is allowed to. Proves 029 §9 G1's "no network call" clause directly,
// rather than by absence of a mock's own comment.
class TripwireSummarizer {
public:
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }

    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest const&, ae::EffectContext&) {
        ++calls;
        co_return ae::ChatResponse{};
    }

    ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest const&, ae::EffectContext&) {
        ae::stream_config<ae::ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = ae::make_stream<ae::ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ae::ChatResponseUpdate upd;  // mirrors chat()'s default-constructed ChatResponse{}
        upd.is_final = true;
        upd.usage    = ae::Usage{};
        auto pushed = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }

    int calls = 0;
};
static_assert(ae::ChatClient<TripwireSummarizer>,
              "TripwireSummarizer must satisfy the ChatClient concept (004 §1)");

using Provider =
    ae::MemoryProvider<TripwireSummarizer, ae::InMemoryWorktreeObjectStore, ae::rt::InMemoryAppendLogStore>;

ae::Message make_msg(ae::role r, std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = r == ae::role::user ? ae::content_origin::user : ae::content_origin::assistant;

    ae::Message m{};
    m.role       = r;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

std::string text_of(ae::Message const& m) {
    if (m.content.empty()) return {};
    if (auto const* t = std::get_if<ae::Text>(&m.content.front().value)) return t->text;
    return {};
}

std::string text_of_content(ae::ContentItem const& item) {
    if (auto const* t = std::get_if<ae::Text>(&item.value)) return t->text;
    return {};
}

// Every field a caller could observe, compared explicitly -- "byte-identical," not "looks close."
bool contribution_identical(ae::ContextContribution const& a, ae::ContextContribution const& b) {
    // `Tainted<T>` deliberately provides no `operator==` (no implicit anything on a security-critical
    // path, tainted.hpp's own doc comment) -- compare explicitly via the same declassifier every
    // other reader of this field must use.
    if (a.instructions.has_value() != b.instructions.has_value()) return false;
    if (a.instructions.has_value() &&
        a.instructions->unsafe_view() != b.instructions->unsafe_view()) return false;
    if (a.messages.size() != b.messages.size()) return false;
    for (std::size_t i = 0; i < a.messages.size(); ++i) {
        ae::Message const& ma = a.messages[i];
        ae::Message const& mb = b.messages[i];
        if (ma.role != mb.role || ma.message_id != mb.message_id) return false;
        if (ma.content.size() != mb.content.size()) return false;
        for (std::size_t j = 0; j < ma.content.size(); ++j) {
            if (ma.content[j].origin != mb.content[j].origin) return false;
            if (ma.content[j].tainted != mb.content[j].tainted) return false;
            if (text_of_content(ma.content[j]) != text_of_content(mb.content[j])) return false;
        }
    }
    if (a.tools.size() != b.tools.size()) return false;
    for (std::size_t i = 0; i < a.tools.size(); ++i) {
        if (a.tools[i].name != b.tools[i].name) return false;
    }
    return true;
}

} // namespace

int main() {
    ae::InMemoryWorktreeObjectStore object_store;
    ae::rt::InMemoryAppendLogStore ref_store;
    ae::Principal const principal{"p-h3", ""};

    auto bootstrapped = ae::ensure_memory_worktree(object_store, ref_store, principal);
    AE_CHECK(bootstrapped.has_value(), "setup: the memory worktree bootstraps");

    ae::Mount const mount = ae::memory_mount(principal);
    ae::cap::FsRead const read_cap{ae::memory_mount_id(principal), "", std::nullopt};
    ae::cap::FsWrite const write_cap{ae::memory_mount_id(principal), "", std::nullopt, std::nullopt};

    ae::MemoryItem tea{};
    tea.kind = ae::memory_kind::episodic;
    tea.content = "the user likes tea in the morning";
    tea.tags = {"preference", "drink"};
    tea.salience = 0.6f;
    tea.origin = ae::MemoryOrigin{ae::memory_source::user_stated, "run-0", "turn-0", principal};
    AE_CHECK(ae::write_memory_item(object_store, ref_store, mount, write_cap, tea).has_value(),
             "setup: writing the first memory item succeeds");

    ae::MemoryItem cmake{};
    cmake.kind = ae::memory_kind::semantic;
    cmake.content = "the build system is CMake";
    cmake.salience = 0.2f;
    cmake.origin = ae::MemoryOrigin{ae::memory_source::tool_derived, "run-0", "turn-0", principal};
    AE_CHECK(ae::write_memory_item(object_store, ref_store, mount, write_cap, cmake).has_value(),
             "setup: writing the second memory item succeeds");

    TripwireSummarizer summarizer;
    Provider provider{object_store, ref_store, mount, read_cap, write_cap, summarizer,
                       /*max_injected=*/5};

    // A FIXED turn -- the same query, asked repeatedly.
    std::vector<ae::Message> const history{make_msg(ae::role::user, "what tea does the user like?", "m-1")};
    ae::EffectContext ctx{};
    ctx.principal = principal;
    ae::SessionContext session_ctx{"s-h3", principal, history};

    // A FIXED memory-worktree tree digest -- captured before any retrieval, checked unchanged after.
    auto ref_before = ae::read_ref(ref_store, ae::memory_ref_name(principal));
    AE_CHECK(ref_before.has_value() && ref_before->has_value(),
             "setup: the memory worktree's Ref is readable before retrieval");

    constexpr int kRepeats = 5;
    std::vector<ae::ContextContribution> runs;
    for (int i = 0; i < kRepeats; ++i) {
        auto out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));
        AE_CHECK(out.has_value(), "H3 setup: on_context() succeeds on repeat " + std::to_string(i));
        if (out.has_value()) runs.push_back(*out);
    }
    AE_CHECK(static_cast<int>(runs.size()) == kRepeats,
             "H3 setup: all " + std::to_string(kRepeats) + " repeats produced a result");

    bool all_identical = true;
    for (std::size_t i = 1; i < runs.size(); ++i) {
        if (!contribution_identical(runs[0], runs[i])) all_identical = false;
    }
    AE_CHECK(all_identical,
             "H3-R1 (the exit-criterion claim): " + std::to_string(kRepeats) + " independent "
             "on_context() calls against the identical fixed memory-worktree state and the "
             "identical fixed turn produce BYTE-IDENTICAL ContextContribution every time -- every "
             "message's role/id/content/taint/origin and every contributed tool's name, not just "
             "message count");

    AE_CHECK(!runs.empty() && !runs[0].messages.empty() &&
                 text_of(runs[0].messages.front()).find(tea.content) != std::string::npos,
             "setup sanity: the tea item, which overlaps the fixed turn's own text, actually ranks "
             "first -- the determinism proof above isn't vacuous over an empty/unranked result "
             "(rendered text now also carries a gap-17 confidence-label prefix ahead of the raw "
             "content, hence a substring check rather than exact equality)");

    AE_CHECK(summarizer.calls == 0,
             "H3-R2: across all " + std::to_string(kRepeats) + " retrieval calls, the declared "
             "ChatClient (summarizer) was invoked ZERO times -- retrieval is a pure read over the "
             "memory worktree, never a network/model call (029 §9 G1's own 'no network call')");

    auto ref_after = ae::read_ref(ref_store, ae::memory_ref_name(principal));
    AE_CHECK(ref_after.has_value() && ref_after->has_value() && ref_before.has_value() &&
                 ref_before->has_value() && (*ref_after)->tree_digest == (*ref_before)->tree_digest,
             "H3-R3: the memory worktree's own tree digest is UNCHANGED after every retrieval call "
             "-- retrieval never writes, so 'a fixed memory-worktree tree digest' in the exit "
             "criterion's own wording is something this test actually holds constant, not merely "
             "assumes");

    // The on-demand recall(query) path is equally deterministic, repeated against the same query.
    auto args = ae::json::parse(R"({"query":"CMake"})");
    AE_CHECK(args.has_value(), "setup: recall args parse");
    std::vector<ae::json::Value> recall_replies;
    for (int i = 0; i < kRepeats; ++i) {
        ae::EffectContext tool_ctx{};
        auto reply = runs[0].tools.front().invoke(*args, tool_ctx);
        AE_CHECK(reply.has_value(), "H3 setup: recall(\"CMake\") repeat " + std::to_string(i) + " succeeds");
        if (reply.has_value()) recall_replies.push_back(*reply);
    }
    bool recall_identical = true;
    for (std::size_t i = 1; i < recall_replies.size(); ++i) {
        if (ae::json::dump(recall_replies[i]) != ae::json::dump(recall_replies[0])) recall_identical = false;
    }
    AE_CHECK(recall_identical && summarizer.calls == 0,
             "H3-R4: the contributed recall(query) tool is likewise deterministic across repeated "
             "identical invocations, and never touches the summarizer either -- the on-demand "
             "lookup path is exactly as replayable as the pre-injected path");

    std::cout << (g_failures == 0 ? "test_memory_retrieval_determinism: OK\n"
                                   : "test_memory_retrieval_determinism: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
