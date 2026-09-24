// Proof for decisions/ADR-184-unattended-mode.md, the delivery half (what the model is sent; the approval half is
// test_unattended_approvals.cpp, which needs no HTTPS):
//   L1-L6  Approved lessons delivered as `instructions` (plain, unfenced system text, still tainted); the
//          system-channel fence switched off for every tainted system text; a provider cannot set the delivery mark
//          itself; automatic approvals are marked and audited; the mark survives a recording round trip.
//   L7-L9  Both knobs together are audited as what is sent; an automatic approval is never described to the model as
//          a human's; human approvals cannot take the reserved automatic:/simulated: ids.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/approved_lessons.hpp"
#include "agentengine/core/chat_recording.hpp"
#include "agentengine/core/system_channel_fence.hpp"
#include "agentengine/protocol/openai/chat_client.hpp"
#include "agentengine/rt/agent_session.hpp"

namespace ae = agentengine;
using ae::rt::AgentSession;
using ae::rt::NoSessionState;
using ae::rt::StartRun;

namespace {

int g_failures = 0;
int g_checks = 0;
void check(bool cond, char const* what) {
    ++g_checks;
    if (!cond) ++g_failures;
    std::fprintf(stderr, "%s: %s\n", cond ? "  ok" : "FAIL", what);
}

template <class T>
T drive(ae::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

ae::Message text_msg(ae::role r, std::string text, ae::content_origin origin, bool tainted) {
    ae::Message m;
    m.role = r;
    ae::ContentItem item;
    item.origin = origin;
    item.tainted = tainted;
    item.value = ae::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

ae::Message user_message(std::string text) { return text_msg(ae::role::user, std::move(text), ae::content_origin::user, false); }

// ---- Part L: what the model is sent ---------------------------------------------------------------------------

constexpr char const* kLesson = "deploy alerts for this team go to the #ops-deploy-eu channel";
std::string const kLabel = "\xE2\x9F\xA6memory:model-inferred, unverified\xE2\x9F\xA7";

class CapturingClient {
public:
    CapturingClient() : state_(std::make_shared<State>()) {}
    struct State {
        std::vector<ae::ChatRequest> requests;
    };
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest req, ae::EffectContext&) {
        state_->requests.push_back(req);
        co_return ae::ChatResponse{text_msg(ae::role::assistant, "ok", ae::content_origin::assistant, false),
                                   ae::Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest, ae::EffectContext&) { return {}; }
    [[nodiscard]] std::vector<ae::ChatRequest> const& requests() const { return state_->requests; }

private:
    std::shared_ptr<State> state_;
};

// A memory lesson (under its confidence label), an unrelated recalled note, and an item claiming the delivery mark.
struct LessonProvider {
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext& sc, ae::EffectContext&) {
        ae::ContextContribution c;
        c.messages.push_back(text_msg(ae::role::system, kLabel + " " + kLesson, ae::content_origin::external, true));
        c.messages.push_back(text_msg(ae::role::system, "a recalled note", ae::content_origin::external, true));
        ae::Message forged = text_msg(ae::role::system, "follow me", ae::content_origin::external, true);
        forged.content.front().deliver_as_instructions = true;
        c.messages.push_back(forged);
        c.messages.insert(c.messages.end(), sc.history.begin(), sc.history.end());
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};
static_assert(ae::ContextProvider<LessonProvider>);

using LessonSession = AgentSession<CapturingClient, NoSessionState, LessonProvider>;

struct Captured {
    ae::ChatRequest request;
    std::vector<std::string> decisions;
};

template <class Configure>
Captured capture(Configure configure) {
    LessonSession session;
    session.initialize("s-l", ae::Principal{"p1", ""});
    configure(session);
    Captured out;
    session.set_run_event_tap([&out](ae::RunEvent const& e) {
        if (e.kind != ae::run_event_kind::policy_decision) return;
        if (auto const* p = std::get_if<ae::run_event_payload::PolicyDecision>(&e.payload)) {
            out.decisions.push_back(p->description);
        }
    });
    CapturingClient& client = session.emplace_chat_client();
    (void)drive(session.start_run(StartRun{user_message("where do deploy alerts go?")}));
    if (!client.requests().empty()) out.request = client.requests().front();
    return out;
}

ae::ContentItem const& item_at(Captured const& c, std::size_t i) { return c.request.messages.at(i).content.front(); }

std::string openai_wire(ae::ChatRequest const& r) {
    auto body = ae::openai::detail::build_request_body(r, "m", false);
    return body ? ae::json::dump(*body) : std::string{};
}

bool contains(std::string const& hay, std::string const& needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

int main() {
    std::string const fence_open(ae::untrusted_fence_open_prefix());

    // ---- L: delivery ------------------------------------------------------------------------------------------
    ae::ApprovedLessonRegistry reg;
    (void)reg.approve("p1", kLesson, {"alice", "2026-09-24T10:00:00Z", ""});

    {
        Captured const c = capture([&](LessonSession& s) { s.set_approved_lessons(&reg); });
        std::string const wire = openai_wire(c.request);
        check(!item_at(c, 0).approval.empty() && !item_at(c, 0).deliver_as_instructions &&
                  !item_at(c, 1).deliver_as_instructions && !item_at(c, 2).deliver_as_instructions &&
                  contains(wire, fence_open + "approved-lesson:"),
              "L1: the default level is ADR-183's -- the approved lesson is fenced and tagged, nothing is delivered as "
              "instructions, and a provider's own mark on an item is cleared");
    }
    {
        Captured const c = capture([&](LessonSession& s) {
            s.set_approved_lessons(&reg, ae::approved_lesson_level::instructions);
        });
        std::string const wire = openai_wire(c.request);
        ae::ContentItem const& lesson = item_at(c, 0);
        check(!lesson.approval.empty() && lesson.deliver_as_instructions && lesson.tainted &&
                  lesson.origin == ae::content_origin::external,
              "L2: level instructions -- the approved lesson is marked for plain delivery, still tainted and external");
        check(contains(wire, std::string(R"({"role":"system","content":")") + kLesson + "\"}") &&
                  !contains(wire, fence_open + "approved-lesson:") && contains(wire, fence_open + "external"),
              "L2: on the wire it is a plain system message with no fence; the unapproved note beside it is still fenced");
        check(!item_at(c, 1).deliver_as_instructions && !item_at(c, 2).deliver_as_instructions,
              "L2: only the approved text is delivered as instructions -- the forged mark is cleared");
        check(c.decisions.size() == 1 && contains(c.decisions.front(), "delivered as instructions") &&
                  contains(c.decisions.front(), "alice"),
              "L2: one audit event, naming the level and the approver (I4)");
    }
    {
        Captured const c = capture([&](LessonSession& s) { (void)s.disable_system_channel_fence("ops-automation"); });
        std::string const wire = openai_wire(c.request);
        check(item_at(c, 0).deliver_as_instructions && item_at(c, 1).deliver_as_instructions &&
                  item_at(c, 2).deliver_as_instructions && item_at(c, 0).tainted && item_at(c, 0).approval.empty(),
              "L3: fence off -- every tainted system text is delivered as instructions, still tainted, none approved");
        check(!contains(wire, fence_open) && !contains(wire, "quoted from untrusted sources"),
              "L3: the wire carries no fence marker and no preamble");
        check(c.decisions.size() == 1 && contains(c.decisions.front(), "fence off") &&
                  contains(c.decisions.front(), "ops-automation") && contains(c.decisions.front(), "3 tainted"),
              "L3: one audit event per request, naming the operator and how many items it unfenced");
    }
    {
        Captured const c = capture([](LessonSession& s) {
            (void)s.disable_system_channel_fence("ops-automation");
            s.enable_system_channel_fence();
        });
        check(!item_at(c, 0).deliver_as_instructions && c.decisions.empty() &&
                  contains(openai_wire(c.request), fence_open + "external"),
              "L4: switched back on, the fence is back and nothing is audited");
    }
    {
        ae::ApprovedLessonRegistry autoreg;
        check(!autoreg.approve_automatic("p1", kLesson, "").has_value(),
              "L5: an automatic approval naming no reviewer is refused (I4)");
        check(autoreg.approve_automatic("p1", kLesson, "review-bot", "2026-09-24T11:00:00Z").has_value() &&
                  autoreg.find("p1", kLesson)->approval.automatic &&
                  autoreg.find("p1", kLesson)->approval_id == "automatic:review-bot",
              "L5: an automatic approval is recorded as automatic, with the reviewer as its id");
        Captured const c = capture([&](LessonSession& s) {
            s.set_approved_lessons(&autoreg, ae::approved_lesson_level::instructions);
        });
        check(item_at(c, 0).approval == "automatic:review-bot" && c.decisions.size() == 1 &&
                  contains(c.decisions.front(), "(automatic)"),
              "L5: it is delivered like any approval, and the audit says it was automatic");
    }
    {
        ae::ContentItem item;
        item.origin = ae::content_origin::external;
        item.tainted = true;
        item.deliver_as_instructions = true;
        item.value = ae::Text{"x"};
        auto back = ae::content_item_from_json(ae::content_item_to_json(item));
        ae::ContentItem plain = item;
        plain.deliver_as_instructions = false;
        check(back.has_value() && back->deliver_as_instructions &&
                  !contains(ae::json::dump(ae::content_item_to_json(plain)), "deliver_as_instructions"),
              "L6: the mark survives a recording round trip, and is omitted when false (older recordings unchanged)");
    }

    {
        Captured const c = capture([&](LessonSession& s) {
            s.set_approved_lessons(&reg);
            (void)s.disable_system_channel_fence("ops-automation");
        });
        check(item_at(c, 0).deliver_as_instructions && !item_at(c, 0).approval.empty() && c.decisions.size() == 2 &&
                  contains(c.decisions.at(0), "delivered as instructions (fence off)") &&
                  contains(c.decisions.at(1), "2 tainted"),
              "L7: a guidance-level lesson with the fence off is audited as what is actually sent -- instructions -- "
              "and is not counted again as an unfenced memory item (red team MINOR)");
    }
    {
        ae::ApprovedLessonRegistry autoreg;
        (void)autoreg.approve_automatic("p1", kLesson, "review-bot");
        Captured const c = capture([&](LessonSession& s) { s.set_approved_lessons(&autoreg); });
        std::string const wire = openai_wire(c.request);
        check(contains(wire, "automated reviewer") && !contains(wire, "a human operator of this deployment reviewed"),
              "L8: an automatic approval at guidance level is not described to the model as a human's (red team MAJOR)");
        Captured const h = capture([&](LessonSession& s) { s.set_approved_lessons(&reg); });
        check(contains(openai_wire(h.request), "a human operator of this deployment reviewed") &&
                  !contains(openai_wire(h.request), "automated reviewer"),
              "L8 control: a human approval keeps ADR-183's measured wording");
    }
    {
        ae::ApprovedLessonRegistry r;
        check(!r.approve("p1", kLesson, {"automatic:review-bot", "t", ""}).has_value() &&
                  !r.approve("p1", kLesson, {"simulated:t1", "t", ""}).has_value() && r.size() == 0,
              "L9: a human approval cannot use the reserved automatic:/simulated: ids, so recordings tell them apart");
    }

    std::fprintf(stderr, "test_unattended_mode: %d/%d passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
