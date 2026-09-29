// Proof for ADR-182 driver phase 1 (decisions/ADR-182-agent-test-driver-mcp.md §12): drives
// tools/test_driver/test_driver.hpp's `Driver` in-process through `handle_line()`, exactly the JSON-RPC
// lines the stdio binary would receive.
//
//   P1-P5 -- protocol: initialize and server/discover answer; tools/list is stable across calls;
//            unknown method, malformed JSON and an oversized line get JSON-RPC errors; a notification
//            gets no reply.
//   C1   -- a send with nothing scripted ends the run with scripted_chat_client.script_exhausted.
//   C3   -- tool arguments cannot change a session's tools or grant: extra fields are ignored, a
//            path-shaped fixture name is refused, the session's model sees exactly the fixture's tools.
//   C3b  -- a fixture that is not compiled in is refused.
//   C4   -- (restated, §12) one gated call: interaction_list shows name, arguments, needs_approval;
//            deny -> no tool_call_started for it; approve -> exactly one, after its approval_requested
//            and (ADR-183, §8's original form) after its approval_resolved.
//   MIX  -- a mixed round lists the gated call needs_approval=true, the free one false.
//   C10  -- session_cancel on a suspended session: no open interaction afterwards, the gated tool
//            never ran, and a following send is accepted.
//   MISC -- send while suspended refused; per-call decisions refused (BUG-2 not fixed); a wait that
//            cannot match times out; max sessions enforced; close discards the session.
//   C8   -- (§19) export stamps each turn's request digest; a replay whose request diverges fails with
//            test.replay_mismatch at that model call. Positive control: a changed user message, which no
//            event shows, passes without digests and fails with them.
//   P4   -- (§21) file fixtures: a committed 015 Agent document starts a session with its instructions,
//            tools and limits; capabilities, unknown tools, untrusted files, unknown extension keys,
//            path-shaped names and shadowing a compiled-in name are refused; the real git check refuses
//            an untracked or modified file in a scratch repository and accepts a committed one.
//   P5   -- (ADR-208) real tools: a shell session writes and reads in its own scratch, records every call,
//            and replays offline with doubles (no sandbox); tampered arguments, results, exec events and
//            exchange counts fail the replay; refusals, per-driver directories, containment, link-safe
//            removal and the caps each have a check.
//   W    -- (ADR-210) workflows: fixture rules; the review loop's routes; the engine's own answers to a bad
//            resolve and a foreign caller; per-step C8 digests naming the step; forced opposite interleavings
//            export identically; drops, cancel, one run, refusals while running, an empty step grant.
//   C2   -- (Windows form) 200 send/observe cycles polling snapshot and events from the MCP thread
//            while the worker runs; every run settles with the expected text. TSan on Linux is the
//            stronger form (named in ADR-182 §13).

#include <cstdio>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "agentengine/pal/env.hpp"
#include "test_driver/fixture_trust.hpp"
#include "test_driver/shell_sandbox.hpp"
#include "test_driver/test_driver.hpp"

namespace td = agentengine::test_driver;
using agentengine::json::Value;

namespace {

int g_failures = 0;

void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  ok: %s\n", what.c_str());
    }
}

int g_next_id = 1;

Value rpc(td::Driver& d, std::string const& method, Value params) {
    Value req = td::obj({{"jsonrpc", td::str("2.0")},
                         {"id", td::num(g_next_id++)},
                         {"method", td::str(method)},
                         {"params", std::move(params)}});
    auto reply = d.handle_line(agentengine::json::dump(req));
    if (!reply) return Value{};
    auto parsed = agentengine::json::parse(*reply);
    return parsed ? *parsed : Value{};
}

struct CallResult {
    bool  is_error = false;
    Value body;  // structuredContent
    std::string error_code;
};

CallResult call(td::Driver& d, std::string const& tool, Value args = td::obj({})) {
    Value r = rpc(d, "tools/call", td::obj({{"name", td::str(tool)}, {"arguments", std::move(args)}}));
    CallResult out;
    Value const* result = r.find("result");
    if (result == nullptr) {
        out.is_error = true;
        out.error_code = "rpc";
        return out;
    }
    if (Value const* e = result->find("isError"); e != nullptr && e->is_bool()) out.is_error = e->as_bool();
    if (Value const* sc = result->find("structuredContent"); sc != nullptr) out.body = *sc;
    if (out.is_error) {
        if (Value const* err = out.body.find("error")) out.error_code = td::get_string(*err, "code").value_or("");
    }
    return out;
}

std::string start(td::Driver& d, std::string const& fixture) {
    CallResult r = call(d, "session_start", td::obj({{"fixture", td::str(fixture)}}));
    return td::get_string(r.body, "session_id").value_or("");
}

Value sid(std::string const& id, td::Members extra = {}) {
    td::Members m{{"session_id", td::str(id)}};
    for (auto& kv : extra) m.push_back(std::move(kv));
    return td::obj(std::move(m));
}

void push(td::Driver& d, std::string const& id, Value turns) {
    CallResult r = call(d, "model_script_push", sid(id, {{"turns", std::move(turns)}}));
    check(!r.is_error, "setup: model_script_push accepted (" + r.error_code + ")");
}

Value text_turn(std::string text) { return td::obj({{"text", td::str(std::move(text))}}); }

// o with key replaced (or added).
Value set_field(Value const& o, std::string const& key, Value v) {
    td::Members m;
    bool done = false;
    for (auto const& [k, x] : o.as_object()) {
        if (k == key) {
            m.emplace_back(k, v);
            done = true;
        } else {
            m.emplace_back(k, x);
        }
    }
    if (!done) m.emplace_back(key, std::move(v));
    return td::obj(std::move(m));
}

Value tool_turn(std::string tool, Value args) {
    return td::obj({{"tool_calls", td::arr({td::obj({{"name", td::str(std::move(tool))}, {"arguments", std::move(args)}})})}});
}

Value call_turn(std::vector<std::pair<std::string, std::string>> calls) {  // (tool, text arg)
    std::vector<Value> cs;
    for (auto& [tool, text] : calls) {
        cs.push_back(td::obj({{"name", td::str(tool)}, {"arguments", td::obj({{"text", td::str(text)}})}}));
    }
    return td::obj({{"tool_calls", td::arr(std::move(cs))}});
}

Value wait(td::Driver& d, std::string const& id, std::string const& until, std::uint64_t timeout_ms = 10000) {
    return call(d, "session_wait_for",
                sid(id, {{"until", td::str(until)}, {"timeout_ms", td::num(static_cast<double>(timeout_ms))}}))
        .body;
}

std::string state_of(Value const& snapshot) { return td::get_string(snapshot, "state").value_or(""); }

std::vector<Value> events_of_kind(td::Driver& d, std::string const& id, std::string const& kind) {
    CallResult r = call(d, "session_events", sid(id, {{"kinds", td::arr({td::str(kind)})}}));
    std::vector<Value> out;
    if (Value const* evs = r.body.find("events"); evs != nullptr && evs->is_array()) out = evs->as_array();
    return out;
}

std::vector<Value> pending_of(td::Driver& d, std::string const& id) {
    CallResult r = call(d, "interaction_list", sid(id));
    std::vector<Value> out;
    if (Value const* p = r.body.find("pending"); p != nullptr && p->is_array()) out = p->as_array();
    return out;
}

std::string outcome_field(Value const& snapshot, std::string const& field) {
    Value const* o = snapshot.find("last_outcome");
    if (o == nullptr) return "";
    return td::get_string(*o, field).value_or("");
}

}  // namespace

int main() {
    // ---- P1-P5: protocol ---------------------------------------------------------------------------
    {
        td::Driver d;
        Value init = rpc(d, "initialize", td::obj({{"protocolVersion", td::str("2025-06-18")}}));
        Value const* res = init.find("result");
        check(res != nullptr && td::get_string(*res, "protocolVersion") == "2025-06-18",
              "P1: initialize answers, echoing the client's protocol version");
        Value inspector = rpc(d, "initialize", td::obj({{"protocolVersion", td::str("2025-11-25")}}));
        check(td::get_string(*inspector.find("result"), "protocolVersion") == "2025-11-25",
              "P1: the MCP Inspector's 2025-11-25 is served (C9)");
        Value unknown = rpc(d, "initialize", td::obj({{"protocolVersion", td::str("1999-01-01")}}));
        check(td::get_string(*unknown.find("result"), "protocolVersion") == std::string(td::kProtocolVersion),
              "P1: an unknown protocol version is answered with the driver's own, not echoed");
        Value disc = rpc(d, "server/discover", td::obj({}));
        check(disc.find("result") != nullptr && disc.find("result")->find("serverInfo") != nullptr,
              "P1: server/discover answers with serverInfo");
        std::string const l1 = agentengine::json::dump(*rpc(d, "tools/list", td::obj({})).find("result"));
        std::string const l2 = agentengine::json::dump(*rpc(d, "tools/list", td::obj({})).find("result"));
        check(l1 == l2 && l1.find("session_start") != std::string::npos, "P2: tools/list is stable across calls");
        Value unk = rpc(d, "no/such", td::obj({}));
        check(unk.find("error") != nullptr, "P3: an unknown method gets a JSON-RPC error");
        auto bad = d.handle_line("{not json");
        check(bad && bad->find("-32700") != std::string::npos, "P4: malformed JSON gets a parse error");
        std::string big(td::kMaxLineBytes + 1, ' ');
        auto over = d.handle_line(big);
        check(over && over->find("-32600") != std::string::npos, "P4: an oversized line is refused");
        auto note = d.handle_line(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
        check(!note.has_value(), "P5: a notification gets no reply");
        CallResult unknown_tool = call(d, "no_such_tool");
        check(unknown_tool.is_error, "P3: an unknown tool is an error");
    }

    // ---- C3b / C3: fixtures and tool arguments ---------------------------------------------------------
    {
        td::Driver d;
        CallResult r1 = call(d, "session_start", td::obj({{"fixture", td::str("my_own_fixture")}}));
        check(r1.is_error && r1.error_code == "test.unknown_fixture", "C3b: a fixture that is not compiled in is refused");
        CallResult r2 = call(d, "session_start", td::obj({{"fixture", td::str("../basic")}}));
        check(r2.is_error && r2.error_code == "test.unknown_fixture", "C3: a path-shaped fixture name is refused");
        CallResult r3 = call(d, "session_start",
                             td::obj({{"fixture", td::str("no_tools")},
                                      {"capabilities", td::arr({td::str("fs.write:/")})},
                                      {"tools", td::arr({td::str("gated_echo")})},
                                      {"suspend_for_approval", td::boolean(false)}}));
        std::string const id = td::get_string(r3.body, "session_id").value_or("");
        check(!r3.is_error && !id.empty(), "C3: extra, authority-shaped arguments do not stop a valid start");
        push(d, id, td::arr({text_turn("ok")}));
        (void)call(d, "session_send", sid(id, {{"text", td::str("hi")}}));
        (void)wait(d, id, "settled");
        CallResult reqs = call(d, "model_requests", sid(id));
        Value const* list = reqs.body.find("requests");
        bool no_tools = list != nullptr && list->is_array() && list->as_array().size() == 1;
        if (no_tools) {
            Value const* tools = list->as_array()[0].find("tools");
            no_tools = tools != nullptr && tools->is_array() && tools->as_array().empty();
        }
        check(no_tools, "C3: the model saw exactly the fixture's tools (none), not the argument's");
    }

    // ---- C1: nothing scripted ---------------------------------------------------------------------------
    {
        td::Driver d;
        std::string const id = start(d, "basic");
        (void)call(d, "session_send", sid(id, {{"text", td::str("hello")}}));
        Value w = wait(d, id, "settled");
        Value const* snap = w.find("snapshot");
        check(snap != nullptr && state_of(*snap) == "idle", "C1: the run settles");
        check(snap != nullptr && outcome_field(*snap, "error_code") == agentengine::testing::kScriptExhaustedCode,
              "C1: the outcome is script_exhausted, not an invented reply");
    }

    // ---- C4: deny ---------------------------------------------------------------------------------------
    {
        td::Driver d;
        std::string const id = start(d, "basic");
        push(d, id, td::arr({call_turn({{"gated_echo", "secret plan"}}), text_turn("understood, not done")}));
        (void)call(d, "session_send", sid(id, {{"text", td::str("do it")}}));
        Value w = wait(d, id, "settled");
        check(w.find("snapshot") && state_of(*w.find("snapshot")) == "suspended", "C4 deny: the run suspends");
        auto pending = pending_of(d, id);
        check(pending.size() == 1, "C4 deny: one pending call");
        std::string ix;
        if (pending.size() == 1) {
            Value const& p = pending[0];
            check(td::get_string(p, "tool_name") == "gated_echo", "C4 deny: the pending call names the tool");
            check(td::get_string(p, "arguments") == R"({"text":"secret plan"})",
                  "C4 deny: the pending call carries its arguments");
            check(td::get_bool(p, "needs_approval") == true, "C4 deny: needs_approval is true");
            ix = td::get_string(p, "interaction_id").value_or("");
        }
        CallResult r = call(d, "interaction_resolve",
                            sid(id, {{"interaction_id", td::str(ix)}, {"decision", td::str("deny")}}));
        check(!r.is_error, "C4 deny: resolve accepted");
        Value w2 = wait(d, id, "settled");
        Value const* snap = w2.find("snapshot");
        check(snap != nullptr && state_of(*snap) == "idle" && outcome_field(*snap, "text") == "understood, not done",
              "C4 deny: the run converges on the scripted follow-up");
        check(events_of_kind(d, id, "tool_call_started").empty(), "C4 deny: no tool_call_started at all");
        check(pending_of(d, id).empty(), "C4 deny: nothing pending afterwards");
    }

    // ---- C4: approve ------------------------------------------------------------------------------------
    {
        td::Driver d;
        std::string const id = start(d, "basic");
        push(d, id, td::arr({call_turn({{"gated_echo", "ship it"}}), text_turn("done")}));
        (void)call(d, "session_send", sid(id, {{"text", td::str("do it")}}));
        (void)wait(d, id, "suspended");
        auto pending = pending_of(d, id);
        std::string const ix = pending.empty() ? "" : td::get_string(pending[0], "interaction_id").value_or("");
        (void)call(d, "interaction_resolve", sid(id, {{"interaction_id", td::str(ix)}, {"decision", td::str("approve")}}));
        Value w = wait(d, id, "idle");
        check(w.find("snapshot") && outcome_field(*w.find("snapshot"), "text") == "done",
              "C4 approve: the run converges");
        auto started = events_of_kind(d, id, "tool_call_started");
        auto requested = events_of_kind(d, id, "approval_requested");
        check(started.size() == 1, "C4 approve: exactly one tool_call_started");
        bool ordered = started.size() == 1 && requested.size() == 1 &&
                       td::get_u64(requested[0], "seq").value_or(0) < td::get_u64(started[0], "seq").value_or(0);
        check(ordered, "C4 approve: tool_call_started comes after its approval_requested");
        auto resolved = events_of_kind(d, id, "approval_resolved");
        check(resolved.size() == 1 && started.size() == 1 &&
                  td::get_u64(resolved[0], "seq").value_or(0) < td::get_u64(started[0], "seq").value_or(0),
              "C4 approve (ADR-183): approval_resolved comes strictly before tool_call_started");
        auto finished = events_of_kind(d, id, "tool_call_finished");
        bool echoed = finished.size() == 1 && finished[0].find("payload") &&
                      td::get_string(*finished[0].find("payload"), "result").value_or("").find("ship it") !=
                          std::string::npos;
        check(echoed, "C4 approve: the gated tool really ran and echoed its argument");
    }

    // ---- MIX: mixed round -------------------------------------------------------------------------------
    {
        td::Driver d;
        std::string const id = start(d, "basic");
        push(d, id, td::arr({call_turn({{"gated_echo", "a"}, {"echo", "b"}}), text_turn("ok")}));
        (void)call(d, "session_send", sid(id, {{"text", td::str("both")}}));
        (void)wait(d, id, "suspended");
        auto pending = pending_of(d, id);
        bool gated = false;
        bool free_listed = false;
        for (Value const& p : pending) {
            if (td::get_string(p, "tool_name") == "gated_echo") gated = td::get_bool(p, "needs_approval") == true;
            if (td::get_string(p, "tool_name") == "echo") free_listed = true;
        }
        // ADR-196 (issue #104 BUG-1): only a call that waits on the decision is named.
        check(pending.size() == 1 && gated && !free_listed,
              "MIX (ADR-196): only the gated call is listed; echo, which needs no approval, is not");
        CallResult per_call = call(d, "interaction_resolve",
                                   sid(id, {{"interaction_id", td::str(pending.empty() ? "" : td::get_string(pending[0], "interaction_id").value_or(""))},
                                            {"decision", td::str("deny")},
                                            {"call_decisions", td::arr({td::obj({{"call_id", td::str("not-a-listed-call")},
                                                                                 {"decision", td::str("approve")}})})}}));
        check(per_call.is_error && per_call.error_code == "test.bad_arguments",
              "MISC (ADR-196): a per-call decision for a call the interaction did not ask about is refused");
        CallResult send_again = call(d, "session_send", sid(id, {{"text", td::str("more")}}));
        check(send_again.is_error && send_again.error_code == "test.session_suspended",
              "MISC: a send while suspended is refused with a clear code");
    }

    // ---- PC: per-call decisions and the approver (ADR-196, issues #104/#108; ADR-182 C7) -------------------
    {
        td::Driver d;
        std::string const id = start(d, "basic");
        push(d, id, td::arr({call_turn({{"gated_echo", "one"}, {"gated_echo", "two"}, {"echo", "free"}}),
                             text_turn("ok")}));
        (void)call(d, "session_send", sid(id, {{"text", td::str("three")}}));
        (void)wait(d, id, "suspended");
        auto pending = pending_of(d, id);
        std::string ix, first, second;
        for (Value const& p : pending) {
            ix = td::get_string(p, "interaction_id").value_or("");
            std::string const args = td::get_string(p, "arguments").value_or("");
            if (args.find("one") != std::string::npos) first = td::get_string(p, "call_id").value_or("");
            if (args.find("two") != std::string::npos) second = td::get_string(p, "call_id").value_or("");
        }
        check(pending.size() == 2 && !first.empty() && !second.empty(), "PC: the two gated calls are listed");
        // A bad approver is refused by the driver before the asynchronous resolve, so the interaction stays open
        // in both the session and the driver's view, and the retry below still works.
        for (char const* bad : {"  ", "alice\nbob"}) {
            CallResult b = call(d, "interaction_resolve",
                                sid(id, {{"interaction_id", td::str(ix)},
                                         {"decision", td::str("approve")},
                                         {"approver_id", td::str(bad)}}));
            check(b.is_error && b.error_code == "test.bad_arguments",
                  "PC (ADR-196): a blank or multi-line approver_id is refused before the resolve runs");
        }
        check(pending_of(d, id).size() == 2, "PC: after a refused approver the interaction is still open");
        // The round is DENIED, except the first call, which is approved -- by a named approver.
        CallResult r = call(d, "interaction_resolve",
                            sid(id, {{"interaction_id", td::str(ix)},
                                     {"decision", td::str("deny")},
                                     {"call_decisions", td::arr({td::obj({{"call_id", td::str(first)},
                                                                          {"decision", td::str("approve")}})})},
                                     {"approver_id", td::str("alice")}}));
        check(!r.is_error, "PC: a per-call resolve is accepted");
        (void)wait(d, id, "idle");
        auto resolved = events_of_kind(d, id, "approval_resolved");
        bool first_yes = false, second_no = false, named = resolved.size() == 2;
        for (Value const& e : resolved) {
            Value const* pl = e.find("payload");
            if (pl == nullptr) continue;
            if (td::get_string(*pl, "approver_id") != "alice") named = false;
            if (td::get_string(*pl, "call_id") == first) first_yes = td::get_bool(*pl, "approved") == true;
            if (td::get_string(*pl, "call_id") == second) second_no = td::get_bool(*pl, "approved") == false;
        }
        check(first_yes && second_no, "PC (#104): each call's approval_resolved carries its own decision");
        check(named, "PC (#108): every approval_resolved names the approver the host supplied");
        auto started = events_of_kind(d, id, "tool_call_started");
        bool ran_first = false, ran_second = false, ran_free = false;
        for (Value const& e : started) {
            Value const* pl = e.find("payload");
            if (pl == nullptr) continue;
            std::string const call_id = td::get_string(*pl, "call_id").value_or("");
            if (call_id == first) ran_first = true;
            if (call_id == second) ran_second = true;
            if (td::get_string(*pl, "tool_name") == "echo") ran_free = true;
        }
        // ADR-182 C7: reintroducing BUG-2 (a round-level deny folded into every call) makes this fail.
        check(ran_first && !ran_second, "PC (#104 BUG-2): the approved call runs, the denied one does not");
        check(ran_free, "PC (#104 BUG-2): the call that never needed approval runs although the round was denied");
    }

    // ---- C10: cancel a suspended session ------------------------------------------------------------------
    {
        td::Driver d;
        std::string const id = start(d, "basic");
        push(d, id, td::arr({call_turn({{"gated_echo", "never"}})}));
        (void)call(d, "session_send", sid(id, {{"text", td::str("go")}}));
        (void)wait(d, id, "suspended");
        CallResult c = call(d, "session_cancel", sid(id));
        check(!c.is_error && td::get_string(c.body, "was") == "suspended", "C10: cancel accepted on a suspended session");
        Value w = wait(d, id, "idle");
        Value const* snap = w.find("snapshot");
        check(snap != nullptr && state_of(*snap) == "idle", "C10: the session returns to idle");
        check(pending_of(d, id).empty(), "C10: no open interaction afterwards");
        check(events_of_kind(d, id, "tool_call_started").empty(), "C10: the gated tool never ran");
        std::fprintf(stderr, "  .. C10 outcome error_code = %s\n",
                     snap ? outcome_field(*snap, "error_code").c_str() : "?");
        push(d, id, td::arr({text_turn("fresh")}));
        CallResult s2 = call(d, "session_send", sid(id, {{"text", td::str("again")}}));
        Value w2 = wait(d, id, "idle");
        check(!s2.is_error && w2.find("snapshot") && outcome_field(*w2.find("snapshot"), "text") == "fresh",
              "C10: a following send is accepted and runs");
    }

    // ---- MISC: waits, limits, close ------------------------------------------------------------------------
    {
        td::Driver d;
        std::string const id = start(d, "basic");
        CallResult w = call(d, "session_wait_for",
                            sid(id, {{"until", td::str("event")}, {"kind", td::str("tool_call_started")},
                                     {"timeout_ms", td::num(50)}}));
        check(!w.is_error && td::get_bool(w.body, "timed_out") == true, "MISC: a wait that cannot match times out");
        for (int i = 0; i < 10; ++i) (void)call(d, "session_start", td::obj({{"fixture", td::str("basic")}}));
        check(d.session_count() == td::kMaxSessions, "MISC: session count is capped");
        CallResult over = call(d, "session_start", td::obj({{"fixture", td::str("basic")}}));
        check(over.is_error && over.error_code == "test.too_many_sessions", "MISC: the next start is refused");
        CallResult closed = call(d, "session_close", sid(id));
        check(!closed.is_error && d.session_count() == td::kMaxSessions - 1, "MISC: close discards the session");
        CallResult gone = call(d, "session_snapshot", sid(id));
        check(gone.is_error && gone.error_code == "test.unknown_session", "MISC: a closed session is unknown");
    }

    // ---- LIVE: live fixtures are a host decision --------------------------------------------------------------
    {
        td::Driver d;  // no live factory: the binary was started without --allow-live
        CallResult r = call(d, "session_start", td::obj({{"fixture", td::str("basic_live")}}));
        check(r.is_error && r.error_code == "test.live_disabled",
              "LIVE: a live fixture is refused unless the host enabled live mode");

        // A stand-in live backend (scripted underneath, no network) to exercise the live-session rules.
        agentengine::testing::ScriptedChatClient fake;
        (void)fake.push(agentengine::testing::text_turn("live says hi"));
        td::DriverConfig cfg;
        cfg.live_description = "fake";
        cfg.live_backend_factory = [fake](std::string const&) -> std::shared_ptr<td::ModelBackend> {
            return std::make_shared<td::ScriptedBackend>(fake);
        };
        td::Driver live(std::move(cfg));
        std::string const id = start(live, "basic_live");
        check(!id.empty(), "LIVE: with live mode enabled, a live fixture starts");
        CallResult push_live = call(live, "model_script_push", sid(id, {{"turns", td::arr({text_turn("x")})}}));
        check(push_live.is_error && push_live.error_code == "test.live_session",
              "LIVE: a live session has no script to push to");
        (void)call(live, "session_send", sid(id, {{"text", td::str("hello")}}));
        Value w = wait(live, id, "idle");
        check(w.find("snapshot") && outcome_field(*w.find("snapshot"), "text") == "live says hi" &&
                  td::get_string(*w.find("snapshot"), "model") == "live",
              "LIVE: the run is answered by the live backend and the snapshot says model=live");
        CallResult reqs = call(live, "model_requests", sid(id));
        check(td::get_u64(reqs.body, "total") == 1u, "LIVE: model_requests captures live calls too");
    }

    // ---- C6: secret canary (positive control first) ------------------------------------------------------------
    {
        std::string const canary = "SEKRET-canary-0123456789";
        auto leak_run = [&](td::Driver& d) {
            std::string const id = start(d, "no_tools");
            push(d, id, td::arr({text_turn("the key is " + canary)}));
            (void)call(d, "session_send", sid(id, {{"text", td::str("leak it")}}));
            std::string const line = agentengine::json::dump(
                td::obj({{"jsonrpc", td::str("2.0")}, {"id", td::num(g_next_id++)}, {"method", td::str("tools/call")},
                         {"params", td::obj({{"name", td::str("session_wait_for")},
                                             {"arguments", sid(id, {{"until", td::str("idle")}})}})}}));
            return d.handle_line(line).value_or("");
        };
        td::Driver open_driver;
        std::string const unfiltered = leak_run(open_driver);
        check(unfiltered.find(canary) != std::string::npos,
              "C6 control: without a canary configured, the scripted text reaches the reply");
        td::DriverConfig cfg;
        cfg.secret_canaries.push_back(canary);
        td::Driver guarded(std::move(cfg));
        std::string const filtered = leak_run(guarded);
        check(filtered.find(canary) == std::string::npos && filtered.find("test.secret_leak_blocked") != std::string::npos,
              "C6: with the canary configured, the reply is withheld and names no content");
        check(guarded.secret_leaks_blocked() == 1, "C6: the block is counted");

        // Scenario files are output too.
        std::filesystem::path const root = std::filesystem::temp_directory_path() / "ae_test_driver_canary";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        td::DriverConfig ecfg;
        ecfg.secret_canaries.push_back(canary);
        ecfg.scenarios_root = root;
        td::Driver exporter(std::move(ecfg));
        std::string const id = start(exporter, "no_tools");
        push(exporter, id, td::arr({text_turn("the key is " + canary)}));
        (void)call(exporter, "session_send", sid(id, {{"text", td::str("leak it")}}));
        (void)call(exporter, "session_wait_for", sid(id, {{"until", td::str("idle")}}));  // reply itself is withheld
        CallResult ex = call(exporter, "scenario_export", sid(id, {{"name", td::str("leaky")}}));
        check(ex.is_error && ex.error_code == "test.secret_leak_blocked" &&
                  !std::filesystem::exists(root / "leaky.json"),
              "C6: a scenario containing the canary is not written");
        std::filesystem::remove_all(root, ec);
    }

    // ---- SCN: scenario export and replay (ADR-182 §16) --------------------------------------------------------
    {
        namespace fs = std::filesystem;
        fs::path const root = fs::temp_directory_path() / "ae_test_driver_scenarios";
        std::error_code ec;
        fs::remove_all(root, ec);

        td::DriverConfig cfg;
        cfg.scenarios_root = root;
        td::Driver d(std::move(cfg));
        std::string const id = start(d, "basic");
        push(d, id, td::arr({call_turn({{"gated_echo", "ship it"}}), text_turn("done")}));
        (void)call(d, "session_send", sid(id, {{"text", td::str("do it")}}));
        (void)wait(d, id, "suspended");
        auto pending = pending_of(d, id);
        std::string const ix = pending.empty() ? "" : td::get_string(pending[0], "interaction_id").value_or("");
        (void)call(d, "interaction_resolve", sid(id, {{"interaction_id", td::str(ix)}, {"decision", td::str("approve")}}));
        (void)wait(d, id, "idle");

        CallResult bad_name = call(d, "scenario_export", sid(id, {{"name", td::str("../escape")}}));
        check(bad_name.is_error && bad_name.error_code == "test.bad_name", "SCN: a path-shaped scenario name is refused");
        CallResult ex = call(d, "scenario_export", sid(id, {{"name", td::str("approve_flow")}}));
        check(!ex.is_error && fs::exists(root / "approve_flow.json"), "SCN: export writes <root>/approve_flow.json");
        CallResult again = call(d, "scenario_export", sid(id, {{"name", td::str("approve_flow")}}));
        check(again.is_error && again.error_code == "test.exists", "SCN: an existing scenario is not overwritten by default");

        CallResult rp = call(d, "scenario_replay", td::obj({{"name", td::str("approve_flow")}}));
        check(!rp.is_error && td::get_bool(rp.body, "passed") == true, "SCN: the exported scenario replays and passes");

        auto scenario = td::read_scenario_file(root / "approve_flow.json");
        check(scenario.has_value(), "SCN: the scenario file parses");
        if (scenario) {
            check(td::replay_scenario(*scenario).passed, "SCN: replay_scenario() passes it too (the runner's path)");
            check(td::replay_scenario(*scenario).passed, "SCN: and again (replay is repeatable)");

            // Positive controls: each tampering must be caught.
            std::string const text = agentengine::json::dump(*scenario);
            auto tampered = [&](std::string const& from, std::string const& to) {
                std::string t = text;
                auto pos = t.find(from);
                if (pos == std::string::npos) return std::optional<Value>{};
                t.replace(pos, from.size(), to);
                auto v = agentengine::json::parse(t);
                return v ? std::optional<Value>(*v) : std::optional<Value>{};
            };
            // expected tool result text lives only in the expected events
            auto t1 = tampered(R"(\"text\":\"ship it\"})", R"(\"text\":\"ship IT\"})");
            check(t1 && !td::replay_scenario(*t1).passed, "SCN control: a changed expected event fails the replay");
            auto t2 = tampered(R"(ship it)", R"(ship them)");  // first occurrence: the model turn's arguments
            check(t2 && !td::replay_scenario(*t2).passed, "SCN control: a changed model turn fails the replay");
            auto t3 = tampered(R"("decision":"approve")", R"("decision":"deny")");
            bool deny_fails = false;
            if (t3) {
                td::ReplayReport const r = td::replay_scenario(*t3);
                deny_fails = !r.passed && !r.problems.empty();
                if (!r.problems.empty()) std::fprintf(stderr, "  .. deny control reports: %s\n", r.problems[0].c_str());
            }
            check(deny_fails, "SCN control: a changed step (approve -> deny) fails the replay with a diff");
        }

        // A cancel that lands while a run is in flight is timing-dependent: not exportable. The run is
        // one text turn, so under load it can finish before the cancel arrives (session_cancel then
        // reports was=idle and nothing is marked). Retry in fresh sessions until one lands mid-run.
        bool landed_running = false;
        bool refused = false;
        for (int attempt = 0; attempt < 50 && !landed_running; ++attempt) {
            std::string const id2 = start(d, "basic");
            push(d, id2, td::arr({text_turn("x")}));
            (void)call(d, "session_send", sid(id2, {{"text", td::str("go")}}));
            CallResult cr = call(d, "session_cancel", sid(id2));
            landed_running = td::get_string(cr.body, "was") == "running";
            (void)wait(d, id2, "settled");
            if (landed_running) {
                CallResult nd = call(d, "scenario_export", sid(id2, {{"name", td::str("cancel_race")}}));
                refused = nd.is_error && nd.error_code == "test.nondeterministic";
            }
            (void)call(d, "session_close", sid(id2));
        }
        check(landed_running && refused, "SCN: a cancel-while-running session is refused");

        // A live session exports as a scripted scenario: the model's observed answers become the script.
        agentengine::testing::ScriptedChatClient fake;
        (void)fake.push({agentengine::testing::tool_calls_turn({{"live_c1", "echo", R"({"text":"from live"})"}}),
                         agentengine::testing::text_turn("live done")});
        td::DriverConfig lcfg;
        lcfg.scenarios_root = root;
        lcfg.live_description = "fake-live";
        lcfg.live_backend_factory = [fake](std::string const&) -> std::shared_ptr<td::ModelBackend> {
            return std::make_shared<td::ScriptedBackend>(fake);
        };
        td::Driver live(std::move(lcfg));
        std::string const lid = start(live, "basic_live");
        (void)call(live, "session_send", sid(lid, {{"text", td::str("echo please")}}));
        (void)wait(live, lid, "idle");
        CallResult lex = call(live, "scenario_export", sid(lid, {{"name", td::str("from_live")}}));
        auto lsc = td::read_scenario_file(root / "from_live.json");
        check(!lex.is_error && lsc && td::get_string(*lsc, "fixture") == "basic",
              "SCN: a live session exports against the scripted twin fixture");
        check(lsc && td::replay_scenario(*lsc).passed, "SCN: the live-derived scenario replays offline and passes");
        check(lsc && td::replay_scenario(*lsc).requests_checked == 2,
              "C8: the live-derived scenario's replay checks both model requests against the live recording");

        td::Driver no_root;
        std::string const id3 = start(no_root, "basic");
        CallResult dis = call(no_root, "scenario_export", sid(id3, {{"name", td::str("x")}}));
        check(dis.is_error && dis.error_code == "test.export_disabled", "SCN: export is disabled without a scenarios root");
        fs::remove_all(root, ec);
    }

    // ---- C8: a replay whose model REQUEST diverges fails with test.replay_mismatch (ADR-182 §19) -----------
    {
        namespace fs = std::filesystem;
        fs::path const root = fs::temp_directory_path() / "ae_test_driver_c8";
        std::error_code ec;
        fs::remove_all(root, ec);
        td::DriverConfig cfg;
        cfg.scenarios_root = root;
        td::Driver d(std::move(cfg));
        std::string const id = start(d, "basic");
        push(d, id, td::arr({call_turn({{"echo", "a"}}), text_turn("done")}));
        (void)call(d, "session_send", sid(id, {{"text", td::str("please echo a")}}));
        (void)wait(d, id, "idle");
        CallResult ex = call(d, "scenario_export", sid(id, {{"name", td::str("c8_flow")}}));
        auto sc = td::read_scenario_file(root / "c8_flow.json");
        bool stamped = sc.has_value() && !ex.is_error;
        if (sc) {
            Value const* turns = sc->find("model_turns");
            stamped = stamped && turns != nullptr && turns->as_array().size() == 2;
            if (stamped) {
                for (Value const& t : turns->as_array()) {
                    stamped = stamped && td::get_string(t, "request_digest").value_or("").starts_with("fnv1a64:");
                }
            }
        }
        check(stamped, "C8: export records the request digest of every model turn");
        if (sc) {
            td::ReplayReport const clean = td::replay_scenario(*sc);
            check(clean.passed && clean.requests_checked == 2, "C8: the untouched scenario replays and checks 2 requests");

            // The user's text is in no event, so an events-only replay cannot see it change. Change it.
            std::string text = agentengine::json::dump(*sc);
            std::string const from = "please echo a";
            auto const pos = text.find(from);
            text.replace(pos, from.size(), "please echo b");
            auto changed = agentengine::json::parse(text);

            // Control: with the digests stripped, the event comparison alone finds nothing (the pre-C8
            // blind spot), and since §22 the missing digests are themselves the failure.
            std::string stripped_text = text;
            for (std::size_t p; (p = stripped_text.find(",\"request_digest\":\"")) != std::string::npos;) {
                std::size_t const end = stripped_text.find('"', p + 19);
                stripped_text.erase(p, end + 1 - p);
            }
            auto stripped = agentengine::json::parse(stripped_text);
            td::ReplayReport const blind = stripped ? td::replay_scenario(*stripped) : td::ReplayReport{};
            check(stripped && !blind.passed && blind.problems.size() == 1 && blind.requests_checked == 0 &&
                      blind.problems[0].find("carry no request_digest") != std::string::npos,
                  "C8 control: without digests the events alone miss the changed message; the missing digests fail (§22)");

            td::ReplayReport const r = changed ? td::replay_scenario(*changed) : td::ReplayReport{};
            bool const named = !r.problems.empty() &&
                               r.problems[0].find("test.replay_mismatch at model call 0") != std::string::npos &&
                               r.problems[0].find("please echo b") != std::string::npos;
            if (!r.problems.empty()) std::fprintf(stderr, "  .. C8 reports: %.200s\n", r.problems[0].c_str());
            check(changed && !r.passed && named,
                  "C8: with digests, the changed request fails as test.replay_mismatch at call 0, showing the request");
        }

        // Directly: a scripted turn that expects another request fails the model call and is not consumed.
        std::string const id2 = start(d, "basic");
        push(d, id2, td::arr({td::obj({{"text", td::str("never said")},
                                       {"request_digest", td::str("fnv1a64:0000000000000000")}})}));
        (void)call(d, "session_send", sid(id2, {{"text", td::str("hi")}}));
        Value w = wait(d, id2, "idle");
        Value const* snap = w.find("snapshot");
        check(snap != nullptr && outcome_field(*snap, "error_code") == "test.replay_mismatch",
              "C8: a request that does not match the turn's digest fails the run with test.replay_mismatch");
        check(snap != nullptr && snap->find("replay_mismatch") != nullptr &&
                  td::get_u64(*snap->find("replay_mismatch"), "call_index") == 0u,
              "C8: the snapshot names the diverging call");
        check(snap != nullptr && td::get_u64(*snap, "script_pending") == 1u, "C8: the turn was not consumed");
        // §22: a mismatch is terminal. Before, the digest queue moved on while the script did not, so a
        // later call was answered by the unconsumed turn with nothing checked.
        push(d, id2, td::arr({text_turn("second")}));
        (void)call(d, "session_send", sid(id2, {{"text", td::str("again")}}));
        Value w3 = wait(d, id2, "idle");
        check(w3.find("snapshot") && outcome_field(*w3.find("snapshot"), "error_code") == "test.replay_mismatch" &&
                  td::get_u64(*w3.find("snapshot"), "script_pending") == 2u,
              "C8 (§22): after a mismatch every later call fails too; no turn is consumed unchecked");
        CallResult ex2 = call(d, "scenario_export", sid(id2, {{"name", td::str("c8_diverged")}}));
        check(ex2.is_error && ex2.error_code == "test.not_recordable", "C8: a diverged session cannot be exported");
        CallResult reqs = call(d, "model_requests", sid(id2));
        Value const* list = reqs.body.find("requests");
        check(list != nullptr && list->is_array() && !list->as_array().empty() &&
                  td::get_string(list->as_array()[0], "digest").value_or("").starts_with("fnv1a64:"),
              "C8: model_requests shows each request's digest");
        fs::remove_all(root, ec);
    }

    // ---- FORK: session_fork (ADR-182 §12 R4, §20) ------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        fs::path const root = fs::temp_directory_path() / "ae_test_driver_fork";
        std::error_code ec;
        fs::remove_all(root, ec);
        td::DriverConfig cfg;
        cfg.scenarios_root = root;
        td::Driver d(std::move(cfg));

        std::string const src = start(d, "basic");
        push(d, src, td::arr({text_turn("r1"), call_turn({{"echo", "two"}}), text_turn("r2")}));
        (void)call(d, "session_send", sid(src, {{"text", td::str("one")}}));
        (void)wait(d, src, "idle");
        (void)call(d, "session_send", sid(src, {{"text", td::str("two")}}));
        (void)wait(d, src, "idle");
        auto history_len = [&](std::string const& id) {
            return td::get_u64(call(d, "session_snapshot", sid(id)).body, "history_length").value_or(999);
        };
        std::uint64_t const src_len = history_len(src);  // user, r1, user, assistant(call), tool, r2

        CallResult f1 = call(d, "session_fork", sid(src, {{"at_turn", td::num(1)}}));
        std::string const b1 = td::get_string(f1.body, "session_id").value_or("");
        check(!f1.is_error && !b1.empty() && td::get_u64(f1.body, "turns_in_source") == 2u,
              "FORK: an idle session forks at turn 1 of 2");
        check(history_len(b1) == 2u, "FORK: the fork keeps exactly turn 1 (user 'one' + 'r1')");
        check(history_len(src) == src_len, "FORK: the source's history is unchanged");
        check(td::get_u64(call(d, "session_snapshot", sid(b1)).body, "script_pending") == 0u,
              "FORK: the fork starts with an empty script (the source's is not shared)");

        // The branch diverges: the model sees turn 1, then the branch's own message, not 'two'.
        push(d, b1, td::arr({text_turn("branch reply")}));
        (void)call(d, "session_send", sid(b1, {{"text", td::str("alternative")}}));
        Value wb = wait(d, b1, "idle");
        check(wb.find("snapshot") && outcome_field(*wb.find("snapshot"), "text") == "branch reply",
              "FORK: the fork runs on its own script");
        CallResult reqs = call(d, "model_requests", sid(b1));
        std::string seen;
        if (Value const* list = reqs.body.find("requests"); list != nullptr && list->is_array() && !list->as_array().empty()) {
            for (Value const& m : list->as_array()[0].find("messages")->as_array())
                seen += td::get_string(m, "role").value_or("") + ":" + td::get_string(m, "text").value_or("") + "|";
        }
        check(seen == "user:one|assistant:r1|user:alternative|",
              "FORK: the fork's first request is turn 1 plus its own message (" + seen + ")");
        check(history_len(src) == src_len, "FORK: running the fork does not touch the source");

        CallResult whole = call(d, "session_fork", sid(src));
        check(!whole.is_error && history_len(td::get_string(whole.body, "session_id").value_or("")) == src_len,
              "FORK: with no at_turn the whole history is copied");
        (void)call(d, "session_close", sid(td::get_string(whole.body, "session_id").value_or("")));
        CallResult zero = call(d, "session_fork", sid(src, {{"at_turn", td::num(0)}}));
        check(!zero.is_error && history_len(td::get_string(zero.body, "session_id").value_or("")) == 0u,
              "FORK: at_turn 0 gives an empty history");
        (void)call(d, "session_close", sid(td::get_string(zero.body, "session_id").value_or("")));
        CallResult too_far = call(d, "session_fork", sid(src, {{"at_turn", td::num(3)}}));
        check(too_far.is_error && too_far.error_code == "test.bad_arguments", "FORK: at_turn past the end is refused");
        CallResult bad_turn = call(d, "session_fork", sid(src, {{"at_turn", td::str("1")}}));
        check(bad_turn.is_error && bad_turn.error_code == "test.bad_arguments", "FORK: a non-integer at_turn is refused");
        CallResult sneaky = call(d, "session_fork", sid(src, {{"fixture", td::str("no_tools")}, {"at_turn", td::num(0)}}));
        std::string const sneaky_id = td::get_string(sneaky.body, "session_id").value_or("");
        check(!sneaky.is_error && td::get_string(call(d, "session_snapshot", sid(sneaky_id)).body, "fixture") == "basic",
              "FORK: the fork always takes the source's fixture (a fixture argument is ignored)");
        (void)call(d, "session_close", sid(sneaky_id));

        // §22: a fork chain is capped (each fork carries its whole ancestry).
        {
            td::Driver deep;
            std::string cur = start(deep, "no_tools");
            std::string last_code;
            int made = 0;
            for (int i = 0; i < 12; ++i) {
                CallResult f = call(deep, "session_fork", sid(cur));
                if (f.is_error) {
                    last_code = f.error_code;
                    break;
                }
                std::string const next = td::get_string(f.body, "session_id").value_or("");
                (void)call(deep, "session_close", sid(cur));
                cur = next;
                ++made;
            }
            check(made == static_cast<int>(td::kMaxForkDepth) && last_code == "test.fork_too_deep",
                  "FORK (§22): a fork chain stops at the depth cap with test.fork_too_deep");
        }

        std::string const sus = start(d, "basic");
        push(d, sus, td::arr({call_turn({{"gated_echo", "x"}})}));
        (void)call(d, "session_send", sid(sus, {{"text", td::str("go")}}));
        (void)wait(d, sus, "suspended");
        CallResult fs_sus = call(d, "session_fork", sid(sus));
        check(fs_sus.is_error && fs_sus.error_code == "test.session_suspended", "FORK: a suspended session is refused");
        (void)call(d, "session_close", sid(sus));

        // Export and replay a fork, and a fork of a fork.
        CallResult ex = call(d, "scenario_export", sid(b1, {{"name", td::str("fork_branch")}}));
        auto sc = td::read_scenario_file(root / "fork_branch.json");
        check(!ex.is_error && sc && td::get_u64(*sc, "format") == 2u && sc->find("segments") != nullptr &&
                  sc->find("segments")->as_array().size() == 1,
              "FORK: a fork exports as format 2 with its ancestry");
        td::ReplayReport const rr = sc ? td::replay_scenario(*sc) : td::ReplayReport{};
        if (!rr.problems.empty()) std::fprintf(stderr, "  .. fork replay: %s\n", rr.problems[0].c_str());
        check(rr.passed && rr.requests_checked == 4, "FORK: the fork's scenario replays (3 ancestor + 1 own request checked)");

        CallResult f2 = call(d, "session_fork", sid(b1, {{"at_turn", td::num(1)}}));
        std::string const b2 = td::get_string(f2.body, "session_id").value_or("");
        push(d, b2, td::arr({text_turn("grandchild")}));
        (void)call(d, "session_send", sid(b2, {{"text", td::str("third way")}}));
        (void)wait(d, b2, "idle");
        CallResult ex2 = call(d, "scenario_export", sid(b2, {{"name", td::str("fork_of_fork")}}));
        auto sc2 = td::read_scenario_file(root / "fork_of_fork.json");
        td::ReplayReport const rr2 = sc2 ? td::replay_scenario(*sc2) : td::ReplayReport{};
        if (!rr2.problems.empty()) std::fprintf(stderr, "  .. fork-of-fork replay: %s\n", rr2.problems[0].c_str());
        check(!ex2.is_error && sc2 && sc2->find("segments")->as_array().size() == 2 && rr2.passed,
              "FORK: a fork of a fork exports two segments and replays");

        // §22: each ancestor is compared with what it recorded. What an ancestor did that never reaches the
        // fork's history -- here its tool call's recorded result, and separately its end state -- was
        // invisible before, because only the final session's events were compared.
        if (sc) {
            auto tamper = [&](std::string const& from, std::string const& to) {
                std::string text = agentengine::json::dump(*sc);
                auto const pos = text.find(from);  // the first occurrence is inside segments[0]
                if (pos == std::string::npos) return td::ReplayReport{};
                text.replace(pos, from.size(), to);
                auto t = agentengine::json::parse(text);
                return t ? td::replay_scenario(*t) : td::ReplayReport{};
            };
            td::ReplayReport const ev = tamper(R"("result":"{\"text\":\"two\"}")", R"("result":"{\"text\":\"TWO\"}")");
            if (!ev.problems.empty()) std::fprintf(stderr, "  .. ancestor control: %.160s\n", ev.problems[0].c_str());
            check(!ev.passed && !ev.problems.empty() && ev.problems[0].starts_with("segment 0 event"),
                  "FORK control (§22): an ancestor event that differs from its recording fails as segment 0");
            td::ReplayReport const st = tamper(R"("fork_at_turn":1,"expected":{"state":"idle")",
                                               R"("fork_at_turn":1,"expected":{"state":"suspended")");
            check(!st.passed && !st.problems.empty() && st.problems[0].starts_with("segment 0 final state"),
                  "FORK control (§22): an ancestor end state that differs fails as segment 0");
        }
        // With the ancestor's digests and events out of the way, a changed ancestor message still reaches
        // the fork's history, which the fork's own first request digest catches.
        if (sc) {
            std::string text = agentengine::json::dump(*sc);
            std::string const from = R"({"op":"send","text":"one"})";
            auto const pos = text.find(from);
            if (pos != std::string::npos) text.replace(pos, from.size(), R"({"op":"send","text":"uno"})");
            auto t = agentengine::json::parse(text);
            td::ReplayReport tr;
            if (t) {
                td::Members top;
                for (auto const& [k, v] : t->as_object()) {
                    if (k != "segments") {
                        top.emplace_back(k, v);
                        continue;
                    }
                    std::vector<Value> segs;
                    for (Value const& seg : v.as_array()) {
                        td::Members sm;
                        for (auto const& [sk, sv] : seg.as_object()) {
                            if (sk == "model_turns") {
                                std::vector<Value> turns;
                                for (Value const& turn : sv.as_array()) {
                                    td::Members tm;
                                    for (auto const& [tk, tv] : turn.as_object())
                                        if (tk != "request_digest") tm.emplace_back(tk, tv);
                                    turns.push_back(td::obj(std::move(tm)));
                                }
                                sm.emplace_back(sk, td::arr(std::move(turns)));
                            } else {
                                sm.emplace_back(sk, sv);
                            }
                        }
                        segs.push_back(td::obj(std::move(sm)));
                    }
                    top.emplace_back(k, td::arr(std::move(segs)));
                }
                tr = td::replay_scenario(td::obj(std::move(top)));
            }
            check(pos != std::string::npos && !tr.passed && !tr.problems.empty() &&
                      tr.problems[0].starts_with("step 0 (send): test.replay_mismatch"),
                  "FORK: a changed ancestor message is caught by the fork's own first request digest");
        }
        // Control: tampering with an ancestor's step fails the replay (here at the ancestor's own digest).
        if (sc) {
            std::string text = agentengine::json::dump(*sc);
            std::string const from = R"({"op":"send","text":"one"})";
            auto const pos = text.find(from);
            if (pos != std::string::npos) text.replace(pos, from.size(), R"({"op":"send","text":"uno"})");
            auto t = agentengine::json::parse(text);
            td::ReplayReport const tr = t ? td::replay_scenario(*t) : td::ReplayReport{};
            check(pos != std::string::npos && !tr.passed && !tr.problems.empty() &&
                      tr.problems[0].find("test.replay_mismatch") != std::string::npos,
                  "FORK control: a changed ancestor step fails the replay with test.replay_mismatch");
        }
        fs::remove_all(root, ec);
    }

    // ---- P4: file fixtures (ADR-182 §21) --------------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        fs::path const root = fs::temp_directory_path() / "ae_test_driver_fixtures";
        fs::path const scen = fs::temp_directory_path() / "ae_test_driver_fixture_scenarios";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::remove_all(scen, ec);
        fs::create_directories(root, ec);
        auto write = [&](std::string const& name, std::string const& text) {
            std::ofstream(root / (name + ".yaml"), std::ios::binary) << text;
        };
        std::string const head = "apiVersion: agentengine.dev/v1\nkind: Agent\nmetadata:\n  id: t\n  description: A test agent.\n";
        write("terse", head + "spec:\n  instructions: Answer in one word.\n  tools:\n    - echo\n  limits:\n    max_turns: 1\n");
        write("grabby", head + "spec:\n  tools:\n    - echo\n  capabilities:\n    net_out: [\"example.com\"]\n");
        write("ghost", head + "spec:\n  tools:\n    - execute_code\n");
        write("dirty", head + "spec:\n  tools:\n    - echo\n");
        write("basic", head + "spec:\n  tools:\n    - fail\n");
        write("no_gate", head + "spec:\n  tools:\n    - gated_echo\nx-test-driver:\n  suspend_for_approval: false\n");
        write("typo", head + "spec:\n  tools:\n    - echo\nx-test-driver:\n  live: true\n");
        write("loose", head + "spec:\n  tools:\n    - echo\n  approval: never_require\n");
        write("scalar", head + "spec:\n  tools: echo\n");

        td::DriverConfig cfg;
        cfg.fixtures_root = root;
        cfg.scenarios_root = scen;
        cfg.fixture_reader = [](fs::path const& p) -> agentengine::result<std::string> {
            if (p.stem() == "dirty") {
                return std::unexpected(agentengine::error{agentengine::failure_class::policy, "has uncommitted changes",
                                                          "test.fixture_untrusted"});
            }
            std::ifstream in(p, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        td::Driver d(std::move(cfg));

        CallResult ok = call(d, "session_start", td::obj({{"fixture", td::str("terse")}}));
        std::string const id = td::get_string(ok.body, "session_id").value_or("");
        check(!ok.is_error && !id.empty(), "P4: a committed file fixture starts a session");
        push(d, id, td::arr({text_turn("Yes.")}));
        (void)call(d, "session_send", sid(id, {{"text", td::str("ok?")}}));
        (void)wait(d, id, "idle");
        CallResult reqs = call(d, "model_requests", sid(id));
        std::string first_role;
        std::string first_text;
        std::string tools;
        if (Value const* list = reqs.body.find("requests"); list != nullptr && list->is_array() && !list->as_array().empty()) {
            Value const& r0 = list->as_array()[0];
            Value const& m0 = r0.find("messages")->as_array()[0];
            first_role = td::get_string(m0, "role").value_or("");
            first_text = td::get_string(m0, "text").value_or("");
            for (Value const& t : r0.find("tools")->as_array()) tools += t.as_string() + ",";
        }
        check(first_role == "system" && first_text.find("Answer in one word.") != std::string::npos,
              "P4: the fixture's instructions reach the model as system text");
        check(tools == "echo,", "P4: the model is offered exactly the fixture's tools (" + tools + ")");

        // max_turns: 1 -- a tool round needs a second model call, which the limit refuses.
        std::string const lim = start(d, "terse");
        push(d, lim, td::arr({call_turn({{"echo", "a"}}), text_turn("never reached")}));
        (void)call(d, "session_send", sid(lim, {{"text", td::str("go")}}));
        Value wl = wait(d, lim, "idle");
        std::fprintf(stderr, "  .. max_turns outcome: %s\n",
                     wl.find("snapshot") ? outcome_field(*wl.find("snapshot"), "error_code").c_str() : "?");
        check(wl.find("snapshot") && outcome_field(*wl.find("snapshot"), "error_code") == "run.max_turns_exceeded" &&
                  td::get_u64(call(d, "session_snapshot", sid(lim)).body, "script_pending") == 1u,
              "P4: the fixture's max_turns limit applies (the second model call never happens)");

        auto refused = [&](std::string const& name) {
            return call(d, "session_start", td::obj({{"fixture", td::str(name)}}));
        };
        CallResult grabby = refused("grabby");
        check(grabby.is_error && grabby.error_code == "test.bad_fixture", "P4: a fixture declaring capabilities is refused");
        CallResult ghost = refused("ghost");
        check(ghost.is_error && ghost.error_code == "test.bad_fixture",
              "P4: a fixture naming a tool that is not a driver test tool is refused");
        CallResult dirty = refused("dirty");
        check(dirty.is_error && dirty.error_code == "test.fixture_untrusted",
              "P4 (C3b): a fixture the host's trust check rejects is refused");
        CallResult typo = refused("typo");
        check(typo.is_error && typo.error_code == "test.bad_fixture", "P4: an unknown x-test-driver key is refused");
        CallResult loose = refused("loose");
        check(loose.is_error && loose.error_code == "test.bad_fixture",
              "P4 (§22): a 015 field the driver does not apply (spec.approval) is refused, not ignored");
        CallResult scalar = refused("scalar");
        check(scalar.is_error && scalar.error_code == "test.bad_fixture",
              "P4 (§22): spec.tools that is not a list is refused, not read as no tools");
        CallResult escape = refused("../terse");
        check(escape.is_error && escape.error_code == "test.unknown_fixture", "P4: a path-shaped fixture name is refused");
        std::string const shadow = start(d, "basic");
        CallResult shadow_list = call(d, "model_requests", sid(shadow));
        push(d, shadow, td::arr({text_turn("x")}));
        (void)call(d, "session_send", sid(shadow, {{"text", td::str("go")}}));
        (void)wait(d, shadow, "idle");
        shadow_list = call(d, "model_requests", sid(shadow));
        std::string shadow_tools;
        if (Value const* list = shadow_list.body.find("requests"); list != nullptr && !list->as_array().empty()) {
            for (Value const& t : list->as_array()[0].find("tools")->as_array()) shadow_tools += t.as_string() + ",";
        }
        check(shadow_tools == "echo,gated_echo,fail,", "P4: a file cannot shadow a compiled-in fixture (" + shadow_tools + ")");

        std::string const ng = start(d, "no_gate");
        push(d, ng, td::arr({call_turn({{"gated_echo", "x"}}), text_turn("after")}));
        (void)call(d, "session_send", sid(ng, {{"text", td::str("go")}}));
        Value wn = wait(d, ng, "settled");
        auto finished = events_of_kind(d, ng, "tool_call_finished");
        bool const refused_call = finished.size() == 1 && finished[0].find("payload") != nullptr &&
                                  td::get_bool(*finished[0].find("payload"), "is_error") == true &&
                                  td::get_string(*finished[0].find("payload"), "result").value_or("").find("approval") !=
                                      std::string::npos;
        check(wn.find("snapshot") && state_of(*wn.find("snapshot")) == "idle" &&
                  events_of_kind(d, ng, "approval_requested").empty() && refused_call,
              "P4: x-test-driver suspend_for_approval: false -- no suspension; the gated call is refused at the "
              "approval step (no decider)");

        CallResult list = call(d, "fixtures_list");
        std::string listed;
        if (Value const* fx = list.body.find("fixtures"); fx != nullptr) {
            for (Value const& f : fx->as_array()) {
                if (td::get_string(f, "source") != "file") continue;
                listed += td::get_string(f, "name").value_or("") + (td::get_bool(f, "available") == true ? "+" : "-") + ",";
            }
        }
        check(listed == "basic-,dirty-,ghost-,grabby-,loose-,no_gate+,scalar-,terse+,typo-,",
              "P4: fixtures_list shows every file fixture, sorted, with whether it loads (" + listed + ")");

        // Export and replay with the fixture; without the fixtures root the replay cannot find it.
        CallResult ex = call(d, "scenario_export", sid(id, {{"name", td::str("terse_flow")}}));
        auto sc = td::read_scenario_file(scen / "terse_flow.json");
        check(!ex.is_error && sc && td::replay_scenario(*sc, td::ReplayFixtures{root, {}}).passed,
              "P4: a file-fixture session exports and replays against the same fixture");
        check(sc && !td::replay_scenario(*sc).passed, "P4: the replay needs the fixtures root");
        // Control: a changed fixture changes the request, which the digest catches.
        write("terse", head + "spec:\n  instructions: Answer in two words.\n  tools:\n    - echo\n  limits:\n    max_turns: 1\n");
        td::ReplayReport const changed = sc ? td::replay_scenario(*sc, td::ReplayFixtures{root, {}}) : td::ReplayReport{};
        check(!changed.passed && !changed.problems.empty() &&
                  changed.problems[0].find("test.replay_mismatch") != std::string::npos,
              "P4 control: editing the fixture's instructions fails the replay with test.replay_mismatch");
        fs::remove_all(root, ec);
        fs::remove_all(scen, ec);
    }

    // ---- P4: the real git trust check, against a throwaway repository -------------------------------------------
    {
        namespace fs = std::filesystem;
        fs::path const repo = fs::temp_directory_path() / "ae_test_driver_git_fixture";
        std::error_code ec;
        fs::remove_all(repo, ec);
        fs::create_directories(repo, ec);
        std::string const q = "\"" + repo.string() + "\"";
#ifdef _WIN32
        std::string const quiet = " >NUL 2>&1";
#else
        std::string const quiet = " >/dev/null 2>&1";
#endif
        bool const have_git = std::system(("git -C " + q + " init -q" + quiet).c_str()) == 0;
        check(have_git, "P4 git: git is available and a scratch repository initializes");
        if (have_git) {
            fs::path const file = repo / "agent.yaml";
            std::string const committed_text = "kind: Agent\nspec: {}\n";
            std::ofstream(file, std::ios::binary) << committed_text;
            auto why = [](agentengine::result<std::string> const& r) { return r ? std::string() : r.error().message; };
            check(why(td::git_committed_fixture(file)) == "not tracked by git",
                  "P4 git: an untracked fixture file is refused");
            int const committed = std::system(("git -C " + q + " add agent.yaml" + quiet).c_str()) |
                                  std::system(("git -C " + q + " -c user.email=t@example.com -c user.name=t "
                                               "-c commit.gpgsign=false commit -q -m init" + quiet).c_str());
            auto const accepted = td::git_committed_fixture(file);
            check(committed == 0 && accepted && *accepted == committed_text,
                  "P4 git: the same file, committed, is accepted and its committed bytes are returned");
            std::ofstream(file, std::ios::binary | std::ios::app) << "# edited\n";
            check(why(td::git_committed_fixture(file)) == "has uncommitted changes",
                  "P4 git: a modified committed file is refused");
            // §22 (red team): assume-unchanged hides the edit from `git diff`. The reader still returns only
            // the committed blob, so the edit is never parsed.
            int const hidden = std::system(("git -C " + q + " update-index --assume-unchanged agent.yaml" + quiet).c_str());
            auto const masked = td::git_committed_fixture(file);
            check(hidden == 0 && masked && *masked == committed_text,
                  "P4 git (§22): with the edit hidden by --assume-unchanged, only the committed bytes are used");
        }
        fs::remove_all(repo, ec);
    }

    // ---- P5: real tools and recorded tool doubles (ADR-208) --------------------------------------------------
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path const base = fs::temp_directory_path() / "ae_test_driver_p5";
        fs::remove_all(base, ec);
        fs::path const sandbox_root = base / "sandbox";
        fs::path const scen_root = base / "scenarios";
        fs::create_directories(scen_root, ec);
        int factory_calls = 0;
        auto config = [&](fs::path const& root) {
            td::DriverConfig c;
            c.scenarios_root = scen_root;
            c.sandbox_root = root;
            c.sandbox_factory = [&factory_calls](fs::path const& dir) {
                ++factory_calls;
                return td::make_shell_sandbox(dir);
            };
            return c;
        };
        auto session_dirs = [](fs::path const& root) {
            std::vector<fs::path> out;
            std::error_code e;
            for (auto const& drv : fs::directory_iterator(root, e)) {
                for (auto const& s : fs::directory_iterator(drv.path(), e)) out.push_back(s.path());
            }
            return out;
        };
        auto finished = [&](td::Driver& d, std::string const& id) { return events_of_kind(d, id, "tool_call_finished"); };
        auto result_of = [](Value const& ev) {
            Value const* pl = ev.find("payload");
            return pl ? td::get_string(*pl, "result").value_or("") : std::string{};
        };
        auto errored = [](Value const& ev) {
            Value const* pl = ev.find("payload");
            return pl != nullptr && td::get_bool(*pl, "is_error") == true;
        };

        // REC: a run_shell write, a read_sandbox_file read, recorded with its exec events.
        {
            td::Driver d(config(sandbox_root));
            std::string const id = start(d, "shell");
            check(!id.empty() && factory_calls == 1, "P5: a shell session starts with one sandbox from the host factory");
            push(d, id, td::arr({tool_turn("run_shell", td::obj({{"source", td::str("echo hello > a.txt")}})),
                                 tool_turn("read_sandbox_file", td::obj({{"path", td::str("a.txt")}})),
                                 text_turn("done")}));
            (void)call(d, "session_send", sid(id, {{"text", td::str("write and read")}}));
            Value w = wait(d, id, "idle");
            auto fin = finished(d, id);
            check(fin.size() == 2 && !errored(fin[0]) && !errored(fin[1]) && result_of(fin[1]).find("hello") != std::string::npos,
                  "P5: run_shell writes a.txt in the session's scratch and read_sandbox_file reads it back");
            check(events_of_kind(d, id, "sandbox_exec_started").size() == 1 &&
                      events_of_kind(d, id, "sandbox_exec_finished").size() == 1,
                  "P5: run_shell's sandbox_exec pair is in the event stream");
            Value const* snap = w.find("snapshot");
            Value const* tc = snap ? snap->find("tool_calls") : nullptr;
            check(tc && td::get_string(*tc, "mode") == "recorded" && td::get_u64(*tc, "made") == 2u,
                  "P5: the snapshot counts two recorded real-tool calls");
            auto dirs = session_dirs(sandbox_root);
            check(dirs.size() == 1 && fs::exists(dirs[0] / "a.txt"), "P5: the file is in <root>/d-*/<session>/");

            CallResult fk = call(d, "session_fork", sid(id));
            check(fk.is_error && fk.error_code == "test.fork_unsupported", "P5 D5: a real-tool session cannot be forked");

            CallResult ex = call(d, "scenario_export", sid(id, {{"name", td::str("p5_roundtrip")}}));
            check(!ex.is_error, "P5: the real-tool session exports");
            CallResult cl = call(d, "session_close", sid(id));
            check(td::get_bool(cl.body, "scratch_removed") == true && !fs::exists(dirs.empty() ? fs::path{} : dirs[0]),
                  "P5 D6: session_close removes the session's scratch directory");
        }
        check(fs::is_directory(sandbox_root) && fs::is_empty(sandbox_root, ec),
              "P5 D6: the driver removes its own directory when it exits");

        auto scenario = td::read_scenario_file(scen_root / "p5_roundtrip.json");
        Value const* tx = scenario ? scenario->find("tool_exchanges") : nullptr;
        check(tx && tx->is_array() && tx->as_array().size() == 2, "P5: the scenario records both real-tool calls");
        if (scenario) {
            int const before = factory_calls;
            td::ReplayReport const r = td::replay_scenario(*scenario);
            check(r.passed, "P5 D1/D8: the scenario replays offline with doubles, the whole event stream compared" +
                                (r.problems.empty() ? std::string{} : ": " + r.problems[0]));
            // Smoke only: a replay's config has no factory, so this count cannot move. D1 itself is structural
            // (the scenario runner does not link the mediated shell; ADR-208 §10).
            check(factory_calls == before && fs::is_empty(sandbox_root, ec),
                  "P5 D1 (smoke): the replay left the sandbox root empty");

            std::string const text = agentengine::json::dump(*scenario);
            std::size_t const tx_at = text.find("\"tool_exchanges\"");
            auto tampered = [&](std::string const& from, std::string const& to) -> std::optional<Value> {
                std::string t = text;
                std::size_t const pos = t.find(from, tx_at);
                if (tx_at == std::string::npos || pos == std::string::npos) return std::nullopt;
                t.replace(pos, from.size(), to);
                auto v = agentengine::json::parse(t);
                return v ? std::optional<Value>(*v) : std::nullopt;
            };
            auto fails_with = [](std::optional<Value> const& sc, std::string const& needle) {
                if (!sc) return false;
                td::ReplayReport const rr = td::replay_scenario(*sc);
                bool const hit = !rr.passed && !rr.problems.empty() && rr.problems[0].find(needle) != std::string::npos;
                if (!rr.problems.empty()) std::fprintf(stderr, "  .. %s\n", rr.problems[0].substr(0, 160).c_str());
                return hit;
            };
            check(fails_with(tampered("echo hello > a.txt", "echo bye > a.txt"), "): test.replay_mismatch at tool call 0: the call differs"),
                  "P5 D2: a recorded call whose arguments differ fails as test.replay_mismatch at tool call 0");
            // D3 edits only exchange 1's result (the read), so the arguments check cannot catch it: the
            // double serves the edited text and the engine's next request to the model differs (C8).
            check(fails_with(tampered("\"content\":\"hello", "\"content\":\"HELLO"), "test.replay_mismatch at model call 2"),
                  "P5 D3: an edited recorded result fails the replay at the next model call");
            // D4: drop the second exchange, or the whole list.
            {
                std::optional<Value> fewer;
                if (tx != nullptr) {
                    std::vector<Value> one{tx->as_array()[0]};
                    fewer = set_field(*scenario, "tool_exchanges", td::arr(one));
                }
                check(fails_with(fewer, "): test.replay_mismatch at tool call 1: the call differs"),
                      "P5 D4: a real-tool call the recording does not have fails the replay");
                std::optional<Value> more;
                if (tx != nullptr) {
                    std::vector<Value> three = tx->as_array();
                    three.push_back(tx->as_array()[1]);
                    more = set_field(*scenario, "tool_exchanges", td::arr(three));
                }
                check(fails_with(more, "never made"), "P5 D4: a recorded call the replay never made fails it");
                check(fails_with(set_field(*scenario, "tool_exchanges", Value{}), "test.real_tools_disabled"),
                      "P5 D4: without its recording a real-tool scenario cannot replay (no sandbox in a replay)");
                check(fails_with(set_field(*scenario, "segments", td::arr({td::obj({})})), "cannot have segments"),
                      "P5 D5: a scenario with both segments and tool_exchanges is refused");
                check(fails_with(set_field(*scenario, "fixture", td::str("basic")), "names no real tool"),
                      "P5 D4 (B3): recorded real-tool calls for a fixture with no real tool are refused, not ignored");
            }
            // D8: drop the recorded exec events and the replay's event stream no longer matches.
            check(fails_with(tampered("\"exec_events\":[{", "\"exec_events\":[],\"x\":[{"), "event"),
                  "P5 D8: a double that does not re-emit the sandbox_exec pair fails the event comparison");
        }

        // D5: no sandbox root, no real tools; fixtures_list says so.
        {
            td::Driver d;
            CallResult r = call(d, "session_start", td::obj({{"fixture", td::str("shell")}}));
            check(r.is_error && r.error_code == "test.real_tools_disabled",
                  "P5 D5: without --sandbox-root a real-tool fixture is refused");
            CallResult fl = call(d, "fixtures_list");
            bool shell_unavailable = false;
            if (Value const* fx = fl.body.find("fixtures"); fx && fx->is_array()) {
                for (Value const& f : fx->as_array()) {
                    if (td::get_string(f, "name") == "shell") shell_unavailable = td::get_bool(f, "available") == false;
                }
            }
            check(shell_unavailable, "P5 D5: fixtures_list shows the shell fixture as unavailable");
        }

        // G3: a copy of the provider (what fork_from makes) never carries the real tools or the sandbox.
        {
            td::DriverHistoryProvider p;
            fs::path const dir = base / "g3";
            fs::create_directories(dir, ec);
            auto sb = td::make_shell_sandbox(dir);
            p.set_real_tools(td::real_tool_standins(), sb ? *sb : nullptr);
            td::DriverHistoryProvider const q(p);
            td::DriverHistoryProvider r;
            r = p;
            check(p.has_real_tools() && !q.has_real_tools() && !r.has_real_tools(),
                  "P5 G3: a copied provider (fork_from's copy) has no real tools and no sandbox");
        }

        // D6: two drivers on one root never share or remove each other's scratch.
        {
            fs::path const shared = base / "shared";
            td::Driver a(config(shared));
            td::Driver b(config(shared));
            std::string const ia = start(a, "shell");
            std::string const ib = start(b, "shell");
            check(ia == ib, "P5 D6 setup: both drivers name their first session the same (" + ia + ")");
            auto write_read = [&](td::Driver& d, std::string const& id, std::string const& word) {
                push(d, id, td::arr({tool_turn("run_shell", td::obj({{"source", td::str("echo " + word + " > f.txt")}})),
                                     text_turn("ok")}));
                (void)call(d, "session_send", sid(id, {{"text", td::str("write")}}));
                (void)wait(d, id, "idle");
            };
            auto read_back = [&](td::Driver& d, std::string const& id) {
                push(d, id, td::arr({tool_turn("read_sandbox_file", td::obj({{"path", td::str("f.txt")}})), text_turn("ok")}));
                (void)call(d, "session_send", sid(id, {{"text", td::str("read")}}));
                (void)wait(d, id, "idle");
                auto fin = finished(d, id);
                return fin.empty() ? std::string{} : result_of(fin.back());
            };
            write_read(a, ia, "alpha");
            write_read(b, ib, "bravo");
            check(read_back(a, ia).find("alpha") != std::string::npos, "P5 D6: driver A reads its own file");
            (void)call(a, "session_close", sid(ia));
            check(read_back(b, ib).find("bravo") != std::string::npos,
                  "P5 D6: driver B still reads its own file after A closed its session of the same name");
        }

        // D7 + G7: containment through the driver, and removal that never follows a link out of the scratch.
        {
            fs::path const root = base / "contain";
            fs::path const outside = base / "outside";
            fs::create_directories(outside, ec);
            { std::ofstream(outside / "keep.txt") << "OUTSIDE-SECRET"; }
            { std::ofstream(base / "secret.txt") << "OUTSIDE-SECRET"; }
            td::Driver d(config(root));
            std::string const id = start(d, "shell");
            auto dirs = session_dirs(root);
            fs::path const scratch = dirs.empty() ? fs::path{} : dirs[0];
            std::vector<std::string> const probes{
                "cat ../secret.txt", "cat ../../secret.txt", "cat ../../../secret.txt", "cat " + (base / "secret.txt").generic_string(),
                "cat NUL", "cat CON", "cat COM1", "cat CONIN$", "cd ..; cat ../secret.txt", "cp ../../../secret.txt x; cat x"};
            std::vector<Value> turns;
            for (std::string const& p : probes) turns.push_back(tool_turn("run_shell", td::obj({{"source", td::str(p)}})));
            turns.push_back(text_turn("done"));
            push(d, id, td::arr(turns));
            (void)call(d, "session_send", sid(id, {{"text", td::str("probe")}}));
            (void)wait(d, id, "idle", 60000);
            bool leaked = false;
            auto fin = finished(d, id);
            for (Value const& ev : fin) leaked = leaked || result_of(ev).find("OUTSIDE-SECRET") != std::string::npos;
            check(fin.size() == probes.size() && !leaked,
                  "P5 D7: ../, absolute paths and device names never read outside the scratch (" + std::to_string(fin.size()) + " probes)");

            // A link to a directory outside, planted by the host (the guest cannot make links), and a deep tree.
            bool linked = false;
            if (!scratch.empty()) {
                fs::create_directory_symlink(outside, scratch / "out_link", ec);
                linked = !ec;
#ifdef _WIN32
                if (!linked) {
                    std::string const cmd = "cmd /c mklink /J \"" + (scratch / "out_link").string() + "\" \"" + outside.string() + "\" >NUL 2>&1";
                    linked = std::system(cmd.c_str()) == 0;
                }
                std::wstring deep = L"\\\\?\\" + fs::absolute(scratch).wstring();
                for (int i = 0; i < 60; ++i) deep += L"\\level_" + std::to_wstring(1000 + i);
                fs::create_directories(fs::path(deep), ec);
                check(!ec && deep.size() > 400, "P5 G7 setup: a directory tree deeper than MAX_PATH exists in scratch");
#endif
            }
            check(linked, "P5 G7 setup: a directory link to outside the root is planted in the scratch");
            CallResult cl = call(d, "session_close", sid(id));
            check(td::get_bool(cl.body, "scratch_removed") == true && !fs::exists(fs::symlink_status(scratch, ec)),
                  "P5 G7: the scratch (with the link and the deep tree) is removed");
            check(fs::exists(outside / "keep.txt"), "P5 G7: removal did not follow the link: the outside file is intact");
        }

        // D10: caps. Planted files stand for what a model could write within its quota.
        {
            fs::path const root = base / "caps";
            td::Driver d(config(root));
            std::string const id = start(d, "shell");
            auto dirs = session_dirs(root);
            fs::path const scratch = dirs.empty() ? fs::path{} : dirs[0];
            { std::ofstream(scratch / "big.txt", std::ios::binary) << std::string(100 * 1024, 'x'); }
            { std::ofstream(scratch / "bin.dat", std::ios::binary) << std::string("\xFF\xFE\x80 not text", 12); }
            {
                std::ofstream f(scratch / "huge.bin", std::ios::binary);
                std::string const mb(1024 * 1024, 'y');
                for (int i = 0; i < 17; ++i) f << mb;
            }
            push(d, id, td::arr({tool_turn("read_sandbox_file", td::obj({{"path", td::str("big.txt")}})),
                                 tool_turn("read_sandbox_file", td::obj({{"path", td::str("bin.dat")}})),
                                 tool_turn("run_shell", td::obj({{"source", td::str("cp big.txt copy.txt")}})),
                                 text_turn("done")}));
            (void)call(d, "session_send", sid(id, {{"text", td::str("caps")}}));
            (void)wait(d, id, "idle", 60000);
            auto fin = finished(d, id);
            check(fin.size() == 3 && errored(fin[0]) && result_of(fin[0]).find("read less") != std::string::npos,
                  "P5 D10: a result over 64 KiB is replaced by an error before the model sees it");
            check(fin.size() == 3 && errored(fin[1]) && result_of(fin[1]).find("UTF-8") != std::string::npos,
                  "P5 D10: a result that is not UTF-8 is replaced by an error");
            check(fin.size() == 3 && result_of(fin[2]).find("No space left") != std::string::npos && !fs::exists(scratch / "copy.txt"),
                  "P5 D10: over the 16 MiB quota, a write is refused (" + (fin.size() == 3 ? result_of(fin[2]).substr(0, 120) : std::string{}) + ")");
        }
        auto slow_script = [] {
            std::string words;
            for (int i = 0; i < 200; ++i) words += " w" + std::to_string(i);
            // This grammar takes no ';' before do/done (`for x in a b do ... done`).
            return "for a in" + words + " do for b in" + words + " do cat one.txt done done";
        };
        // D10 (judge §10): the 1 MiB read cap and the 2 s wall clock reach the shell.
        {
            fs::path const root = base / "limits";
            td::Driver d(config(root));
            std::string const id = start(d, "shell");
            auto dirs = session_dirs(root);
            fs::path const scratch = dirs.empty() ? fs::path{} : dirs[0];
            { std::ofstream(scratch / "mid.txt", std::ios::binary) << std::string(1536 * 1024, 'm'); }
            { std::ofstream(scratch / "one.txt", std::ios::binary) << std::string(1000 * 1024, 'o'); }
            // 40,000 reads of ~1 MB: far past the 10 s default if it ran to the end. The budget is checked
            // between statements. Nested loops keep the script small (a long flat script trips #147 in Debug).
            std::string const slow = slow_script() + "; echo finished";
            push(d, id, td::arr({tool_turn("run_shell", td::obj({{"source", td::str("cat mid.txt")}})),
                                 tool_turn("run_shell", td::obj({{"source", td::str(slow)}})), text_turn("done")}));
            auto const t0 = std::chrono::steady_clock::now();
            (void)call(d, "session_send", sid(id, {{"text", td::str("limits")}}));
            (void)wait(d, id, "idle", 60000);
            auto const secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            auto fin = finished(d, id);
            std::string const r0 = fin.size() == 2 ? result_of(fin[0]) : std::string{};
            std::string const r1 = fin.size() == 2 ? result_of(fin[1]) : std::string{};
            std::fprintf(stderr, "  .. read cap: %s\n  .. wall clock (%.1f s): %s\n", r0.substr(0, 160).c_str(), secs,
                         r1.substr(0, 160).c_str());
            check(fin.size() == 2 && r0.find("\"ok\":false") != std::string::npos && r0.find("mmmm") == std::string::npos,
                  "P5 D10: a 1.5 MiB file is refused by the shell's 1 MiB read cap");
            check(fin.size() == 2 && r1.find("finished") == std::string::npos && r1.find("bounds") == std::string::npos &&
                      secs >= 1.5 && secs < 8.0,
                  "P5 D10: a long script stops at the 2 s wall clock, not the 10 s default");
        }
        // G2 end to end: a cancel during one real-tool call stops the next. The engine's loop already ends the
        // run on cancel before dispatching it; the wrapper's own cancel check (below) is a second layer.
        {
            fs::path const root = base / "cancel";
            td::Driver d(config(root));
            std::string const id = start(d, "shell");
            auto dirs0 = session_dirs(root);
            if (!dirs0.empty()) std::ofstream(dirs0[0] / "one.txt", std::ios::binary) << std::string(1000 * 1024, 'o');
            std::string const slow = slow_script();
            push(d, id, td::arr({tool_turn("run_shell", td::obj({{"source", td::str(slow)}})),
                                 tool_turn("run_shell", td::obj({{"source", td::str("echo second > second.txt")}})),
                                 text_turn("done")}));
            (void)call(d, "session_send", sid(id, {{"text", td::str("cancel me")}}));
            (void)call(d, "session_wait_for", sid(id, {{"until", td::str("event")}, {"kind", td::str("sandbox_exec_started")},
                                                       {"timeout_ms", td::num(10000)}}));
            (void)call(d, "session_cancel", sid(id));
            (void)wait(d, id, "settled", 30000);
            auto dirs = session_dirs(root);
            bool const second_ran = !dirs.empty() && fs::exists(dirs[0] / "second.txt");
            auto fin = finished(d, id);
            std::fprintf(stderr, "  .. cancel: %zu finished, first: %s\n", fin.size(),
                         fin.empty() ? "" : result_of(fin[0]).substr(0, 120).c_str());
            check(!dirs.empty() && !second_ran && fin.size() == 1 && result_of(fin[0]).find("bounds") == std::string::npos,
                  "P5 G2: a session_cancel during a real-tool call stops the next real-tool call (engine loop)");
        }
        {
            // The call cap and the cancel check, on the wrapper itself (a fake inner tool counts real calls).
            auto log = std::make_shared<td::RealToolLog>(std::nullopt);
            int inner_calls = 0;
            agentengine::ToolDescriptor fake = td::real_tool_standins()[0];
            fake.invoke = [&inner_calls](Value const&, agentengine::EffectContext&) -> agentengine::result<Value> {
                ++inner_calls;
                return td::obj({{"ok", td::boolean(true)}});
            };
            agentengine::ToolDescriptor const wrapped = td::recording_tool(fake, log, nullptr);
            agentengine::EffectContext ctx;
            bool capped = false;
            for (std::size_t i = 0; i <= td::kMaxRealToolCalls; ++i) {
                auto r = wrapped.invoke(td::obj({}), ctx);
                if (!r) capped = r.error().code == "test.real_tool_call_cap";
            }
            check(capped && inner_calls == static_cast<int>(td::kMaxRealToolCalls),
                  "P5 D10: past the per-session call cap the real tool is not called");
            auto log2 = std::make_shared<td::RealToolLog>(std::nullopt);
            agentengine::ToolDescriptor const wrapped2 = td::recording_tool(fake, log2, nullptr);
            std::stop_source stop;
            agentengine::EffectContext cctx;
            cctx.cancellation = stop.get_token();
            stop.request_stop();
            int const before = inner_calls;
            auto r = wrapped2.invoke(td::obj({}), cctx);
            check(!r && r.error().code == "test.real_tool_canceled" && inner_calls == before,
                  "P5 G2: after cancel no real-tool call runs");
            check(td::valid_utf8("plain \xC3\xA9 text") && !td::valid_utf8("\xFF") && !td::valid_utf8("\xC3") &&
                      !td::valid_utf8("\xED\xA0\x80"),
                  "P5: the UTF-8 check accepts text and refuses stray, truncated and surrogate bytes");
        }
        fs::remove_all(base, ec);
    }

    // ---- W: the workflow tool group (ADR-210) ----------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path const wbase = fs::temp_directory_path() / "ae_test_driver_wf";
        fs::remove_all(wbase, ec);
        fs::create_directories(wbase, ec);
        td::DriverConfig wcfg;
        wcfg.scenarios_root = wbase;
        auto wid = [](std::string const& id, td::Members extra = {}) {
            td::Members m{{"workflow_id", td::str(id)}};
            for (auto& kv : extra) m.push_back(std::move(kv));
            return td::obj(std::move(m));
        };
        auto wstart = [&](td::Driver& d, std::string const& fixture) {
            CallResult r = call(d, "workflow_start", td::obj({{"fixture", td::str(fixture)}}));
            return td::get_string(r.body, "workflow_id").value_or("");
        };
        auto wpush = [&](td::Driver& d, std::string const& w, std::string const& step, Value turns) {
            CallResult r = call(d, "workflow_script_push", wid(w, {{"executor_id", td::str(step)}, {"turns", std::move(turns)}}));
            check(!r.is_error, "W setup: workflow_script_push to " + step + " accepted (" + r.error_code + ")");
        };
        auto wsettle = [&](td::Driver& d, std::string const& w) {
            CallResult r = call(d, "workflow_wait_for", wid(w, {{"until", td::str("settled")}, {"timeout_ms", td::num(30000)}}));
            Value const* snap = r.body.find("snapshot");
            return snap != nullptr ? *snap : Value{};
        };
        auto result_of = [](Value const& snap, std::string const& key) {
            Value const* r = snap.find("last_result");
            return r != nullptr ? td::get_string(*r, key).value_or("") : std::string{};
        };
        auto open_ports = [&](td::Driver& d, std::string const& w) {
            std::vector<std::string> ids;
            CallResult r = call(d, "request_port_list", wid(w));
            if (Value const* o = r.body.find("open"); o != nullptr && o->is_array()) {
                for (Value const& x : o->as_array()) ids.push_back(td::get_string(x, "interaction_id").value_or(""));
            }
            return ids;
        };
        auto resolve = [&](td::Driver& d, std::string const& w, std::string const& ix, std::string const& text,
                           std::vector<std::string> routes, std::string const& caller = {}) {
            std::vector<Value> rs;
            for (auto& r : routes) rs.push_back(td::str(r));
            td::Members m{{"interaction_id", td::str(ix)}, {"text", td::str(text)}, {"routes", td::arr(std::move(rs))}};
            if (!caller.empty()) m.emplace_back("caller", td::str(caller));
            CallResult r = call(d, "request_port_resolve", wid(w, std::move(m)));
            check(!r.is_error, "W setup: request_port_resolve accepted (" + r.error_code + ")");
            return wsettle(d, w);
        };
        auto structural_kinds = [&](td::Driver& d, std::string const& w) {
            std::vector<std::string> kinds;
            CallResult r = call(d, "workflow_events", wid(w, {{"limit", td::num(200)}}));
            if (Value const* e = r.body.find("events"); e != nullptr && e->is_array()) {
                for (Value const& x : e->as_array()) kinds.push_back(td::get_string(x, "kind").value_or(""));
            }
            return kinds;
        };
        auto count_of = [](std::vector<std::string> const& v, std::string const& k) {
            return static_cast<std::size_t>(std::count(v.begin(), v.end(), k));
        };

        // Fixture rules (W7): every compiled fixture passes; a gated tool, a real tool, a deadline or no bound is refused.
        {
            bool all_ok = true;
            for (td::WorkflowFixture const& f : td::workflow_fixtures()) all_ok = all_ok && td::check_workflow_fixture(f).has_value();
            check(all_ok, "W7: every compiled workflow fixture meets the fixture rules");
            td::WorkflowFixture gated = td::workflow_fixtures()[0];
            gated.agent_tools["draft"] = {"gated_echo"};
            auto g = td::check_workflow_fixture(gated);
            check(!g && g.error().message.find("needs approval") != std::string::npos,
                  "W7: an agent step naming a gated tool is refused (its approval could never reach the tester)");
            td::WorkflowFixture real = td::workflow_fixtures()[0];
            real.agent_tools["draft"] = {"run_shell"};
            auto rr = td::check_workflow_fixture(real);
            check(!rr && rr.error().message.find("real tool") != std::string::npos, "W7: a real tool in a step is refused");
            td::WorkflowFixture timed = td::workflow_fixtures()[0];
            timed.graph.bound.deadline_ms = 1000;
            check(!td::check_workflow_fixture(timed), "W7: a wall-clock deadline is refused (nondeterministic)");
            td::WorkflowFixture unbounded = td::workflow_fixtures()[0];
            unbounded.graph.bound.max_rounds = 64;
            check(!td::check_workflow_fixture(unbounded), "W8: a fixture over the round cap is refused");
        }

        // W1: the review loop. approve runs publish; revise runs draft again and reopens the port.
        std::string const kPort1 = "wf_review:run:1:port:review:1";
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_review");
            check(!w.empty(), "W1: wf_review starts");
            wpush(d, w, "draft", td::arr({tool_turn("echo", td::obj({{"text", td::str("outline")}})), text_turn("draft v1")}));
            wpush(d, w, "publish", td::arr({text_turn("published v1")}));
            check(!call(d, "workflow_run", wid(w, {{"text", td::str("write a note")}})).is_error, "W1: workflow_run starts the run");
            Value s1 = wsettle(d, w);
            auto ports = open_ports(d, w);
            check(td::get_string(s1, "state") == "suspended" && ports.size() == 1 && ports[0] == kPort1,
                  "W1: the run suspends at the review port; request_port_list shows exactly its id (" +
                      (ports.empty() ? std::string("none") : ports[0]) + ")");
            CallResult listed = call(d, "request_port_list", wid(w));
            Value const* open = listed.body.find("open");
            check(open != nullptr && !open->as_array().empty() &&
                      td::get_string(open->as_array()[0], "ask").value_or("").find("draft v1") != std::string::npos &&
                      td::get_string(open->as_array()[0], "port") == "review",
                  "W1: the ask is draft's output, and the port is named");
            // W11: nothing may start while... (it is suspended, not running: a second run is refused for another reason)
            CallResult again = call(d, "workflow_run", wid(w, {{"text", td::str("again")}}));
            check(again.is_error && again.error_code == "test.workflow_already_run", "W11: a workflow runs once");
            Value s2 = resolve(d, w, kPort1, "ship it", {"approve"});
            check(td::get_string(s2, "state") == "finished" && result_of(s2, "status") == "completed" &&
                      result_of(s2, "output") == "published v1",
                  "W1: route approve runs publish and the run completes with its output (" + result_of(s2, "output") + ")");
            auto kinds = structural_kinds(d, w);
            check(count_of(kinds, "request_port_opened") == 1 && count_of(kinds, "request_port_resolved") == 1 &&
                      count_of(kinds, "route_selected") == 1,
                  "W1: the structural log shows one port opened, resolved and routed");
            CallResult draft_ev = call(d, "workflow_events", wid(w, {{"executor_id", td::str("draft")}}));
            Value const* dev = draft_ev.body.find("events");
            bool has_tool = false;
            if (dev != nullptr) {
                for (Value const& e : dev->as_array()) has_tool = has_tool || td::get_string(e, "kind") == "tool_call_finished";
            }
            check(has_tool, "W1: draft's own log has its echo call (the step's run events are kept per step)");
            CallResult ex = call(d, "scenario_export", wid(w, {{"name", td::str("wf_review_approve")}}));
            check(!ex.is_error, "W4: the workflow exports (" + ex.error_code + ")");
            // W7: a step's session is not a driver session.
            CallResult reach = call(d, "session_send", sid("wf_review/draft", {{"text", td::str("hi")}}));
            check(reach.is_error && reach.error_code == "test.unknown_session", "W7: a step's session is unreachable by session tools");
        }
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_review");
            wpush(d, w, "draft", td::arr({text_turn("draft v1"), text_turn("draft v2")}));
            wpush(d, w, "publish", td::arr({text_turn("published v2")}));
            (void)call(d, "workflow_run", wid(w, {{"text", td::str("write")}}));
            (void)wsettle(d, w);
            Value s2 = resolve(d, w, kPort1, "tighten it", {"revise"});
            auto ports = open_ports(d, w);
            check(td::get_string(s2, "state") == "suspended" && ports.size() == 1 && ports[0] != kPort1,
                  "W1: route revise runs draft again and the port reopens with a new, round-qualified id (" +
                      (ports.empty() ? std::string("none") : ports[0]) + ")");
            Value s3 = resolve(d, w, ports.empty() ? std::string{} : ports[0], "ok", {"approve"});
            check(result_of(s3, "status") == "completed" && result_of(s3, "output") == "published v2",
                  "W1: the second answer approves and the run completes");
        }

        // W2: the engine decides a bad resolve; the driver passes it through unchanged.
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_review");
            wpush(d, w, "draft", td::arr({text_turn("draft")}));
            wpush(d, w, "publish", td::arr({text_turn("done")}));
            (void)call(d, "workflow_run", wid(w, {{"text", td::str("go")}}));
            (void)wsettle(d, w);
            auto before = structural_kinds(d, w);
            Value s = resolve(d, w, "wf_review:run:1:port:review:9", "x", {"approve"});
            auto after = structural_kinds(d, w);
            check(result_of(s, "status") == "invalid" && td::get_string(s, "state") == "suspended" && open_ports(d, w).size() == 1,
                  "W2: an unknown id is invalid and the port stays open");
            check(count_of(after, "workflow_run_failed") == count_of(before, "workflow_run_failed") + 1,
                  "W2: ... and the engine logs one workflow_run_failed while the run stays suspended (engine finding)");
            Value bad = resolve(d, w, kPort1, "x", {"bogus"});
            check(result_of(bad, "status") == "routing_failed" && result_of(bad, "failed_executor") == "review" &&
                      td::get_string(bad, "state") == "finished",
                  "W2: an invented-only route consumes the port and ends routing_failed at review");
        }
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_review");
            wpush(d, w, "draft", td::arr({text_turn("draft")}));
            wpush(d, w, "publish", td::arr({text_turn("done")}));
            (void)call(d, "workflow_run", wid(w, {{"text", td::str("go")}}));
            (void)wsettle(d, w);
            Value mixed = resolve(d, w, kPort1, "x", {"approve", "bogus"});
            check(result_of(mixed, "status") == "completed",
                  "W2: a mixed route (one declared case plus an invented one) is accepted by the engine");
        }
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_two_ports");
            (void)call(d, "workflow_run", wid(w, {{"text", td::str("in")}}));
            (void)wsettle(d, w);
            auto ports = open_ports(d, w);
            check(ports.size() == 2, "W2: two ports open in the same round (" + std::to_string(ports.size()) + ")");
            std::string p1;
            std::string p2;
            for (auto const& p : ports) (p.find(":port:p1:") != std::string::npos ? p1 : p2) = p;
            Value first = resolve(d, w, p1, "x", {"bogus"});
            check(result_of(first, "status") == "suspended" && td::get_string(first, "state") == "suspended",
                  "W2: with two ports, a bad route on p1 is stored and the run stays suspended");
            Value second = resolve(d, w, p2, "y", {});
            check(result_of(second, "status") == "routing_failed" && result_of(second, "failed_executor") == "p1",
                  "W2: ... and the failure surfaces on p2's resolve, naming p1");
        }
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_two_ports");
            (void)call(d, "workflow_run", wid(w, {{"text", td::str("in")}}));
            (void)wsettle(d, w);
            std::string p1;
            std::string p2;
            for (auto const& p : open_ports(d, w)) (p.find(":port:p1:") != std::string::npos ? p1 : p2) = p;
            (void)resolve(d, w, p1, "a", {"go"});
            Value done = resolve(d, w, p2, "b", {});
            check(result_of(done, "status") == "completed" && result_of(done, "output").find(">join") != std::string::npos,
                  "W1: both ports answered, each routed to its own branch, fan-in to join (" + result_of(done, "output") + ")");
            CallResult ex = call(d, "scenario_export", wid(w, {{"name", td::str("wf_two_ports_go")}}));
            check(!ex.is_error, "W4: a workflow with no agent steps exports too");
        }

        // W3: another caller is refused by the engine's admission; the port stays open; the owner then answers.
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_review");
            wpush(d, w, "draft", td::arr({text_turn("draft")}));
            wpush(d, w, "publish", td::arr({text_turn("done")}));
            (void)call(d, "workflow_run", wid(w, {{"text", td::str("go")}}));
            (void)wsettle(d, w);
            Value denied = resolve(d, w, kPort1, "x", {"approve"}, "mallory");
            check(result_of(denied, "status") == "admission_denied" && td::get_string(denied, "state") == "suspended" &&
                      open_ports(d, w).size() == 1 && td::get_u64(denied, "admission_denied") == 1u,
                  "W3: another caller is admission_denied, the port stays open and the denial is counted");
            CallResult bad_id = call(d, "request_port_resolve",
                                     wid(w, {{"interaction_id", td::str(kPort1)}, {"text", td::str("x")}, {"caller", td::str("ab")}}));
            check(bad_id.is_error && bad_id.error_code == "test.bad_arguments", "W3: a caller that is not an attributable id is refused");
            Value ok = resolve(d, w, kPort1, "x", {"approve"});
            check(result_of(ok, "status") == "completed", "W3: the owner then resolves the same port");
        }

        // W4: the exported scenario replays; a changed request names the step it came from.
        {
            auto sc = td::read_scenario_file(wbase / "wf_review_approve.json");
            check(sc.has_value(), "W4: the exported workflow scenario reads back");
            if (sc) {
                td::ReplayReport const r = td::replay_scenario(*sc);
                check(r.passed && r.requests_checked == 3,
                      "W4: it replays: structural stream, per-step logs and result match, 3 requests checked (" +
                          (r.problems.empty() ? std::to_string(r.requests_checked) : r.problems[0]) + ")");
                std::string const text = agentengine::json::dump(*sc);
                auto tampered = [&](std::string const& from, std::string const& to) -> std::optional<Value> {
                    std::string t = text;
                    std::size_t const pos = t.find(from);
                    if (pos == std::string::npos) return std::nullopt;
                    t.replace(pos, from.size(), to);
                    auto v = agentengine::json::parse(t);
                    return v ? std::optional<Value>(*v) : std::nullopt;
                };
                auto first_problem = [](std::optional<Value> const& s) {
                    if (!s) return std::string("(tamper did not apply)");
                    td::ReplayReport const rr = td::replay_scenario(*s);
                    return rr.problems.empty() ? std::string("(passed)") : rr.problems[0];
                };
                std::string const p_pub = first_problem(tampered("\"text\":\"ship it\"", "\"text\":\"ship them\""));
                check(p_pub.find("test.replay_mismatch at publish model call 0") != std::string::npos,
                      "W4: a changed answer changes publish's request, and the failure names publish (" + p_pub.substr(0, 90) + ")");
                std::string const p_draft = first_problem(tampered("\"text\":\"write a note\"", "\"text\":\"write a poem\""));
                check(p_draft.find("test.replay_mismatch at draft model call 0") != std::string::npos,
                      "W4: a changed run input changes draft's request, and the failure names draft (" + p_draft.substr(0, 90) + ")");
                std::string const p_route = first_problem(tampered("\"routes\":[\"approve\"]", "\"routes\":[\"revise\"]"));
                check(p_route.find("differs") != std::string::npos || p_route.find("replay_mismatch") != std::string::npos,
                      "W4: a changed route fails the replay (" + p_route.substr(0, 90) + ")");
            }
            auto two = td::read_scenario_file(wbase / "wf_two_ports_go.json");
            check(two && td::replay_scenario(*two).passed, "W4: the two-port scenario replays");
        }

        // W5: forced opposite interleavings of the two parallel steps export identically.
        {
            struct Latch {
                std::mutex              m;
                std::condition_variable cv;
                bool                    first_called = false;
            };
            class LatchBackend final : public td::ModelBackend {
            public:
                LatchBackend(std::shared_ptr<td::ModelBackend> inner, std::shared_ptr<Latch> latch, bool first)
                    : inner_(std::move(inner)), latch_(std::move(latch)), first_(first) {}
                agentengine::ChatClientCapabilities capabilities() const override { return inner_->capabilities(); }
                agentengine::rt::task<agentengine::result<agentengine::ChatResponse>> chat(agentengine::ChatRequest const& r,
                                                                                          agentengine::EffectContext& ctx) override {
                    if (!first_) {
                        std::unique_lock lock(latch_->m);
                        latch_->cv.wait_for(lock, std::chrono::seconds(5), [&] { return latch_->first_called; });
                    }
                    auto out = co_await inner_->chat(r, ctx);
                    if (first_) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        {
                            std::lock_guard lock(latch_->m);
                            latch_->first_called = true;
                        }
                        latch_->cv.notify_all();
                    }
                    co_return out;
                }
                agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(agentengine::ChatRequest const& r,
                                                                                 agentengine::EffectContext& ctx) override {
                    return inner_->chat_stream(r, ctx);
                }

            private:
                std::shared_ptr<td::ModelBackend> inner_;
                std::shared_ptr<Latch>            latch_;
                bool                              first_;
            };
            auto run_ordered = [&](std::string const& first_step, std::string const& name) {
                td::DriverConfig c = wcfg;
                auto latch = std::make_shared<Latch>();
                c.workflow_backend_wrapper = [latch, first_step](std::string const& step, std::shared_ptr<td::ModelBackend> inner) {
                    return std::shared_ptr<td::ModelBackend>(std::make_shared<LatchBackend>(std::move(inner), latch, step == first_step));
                };
                td::Driver d(c);
                std::string const w = wstart(d, "wf_fanout");
                for (std::string const step : {"a", "b"}) {
                    wpush(d, w, step, td::arr({tool_turn("echo", td::obj({{"text", td::str(step + "-note")}})), text_turn(step + " done")}));
                }
                (void)call(d, "workflow_run", wid(w, {{"text", td::str("split this")}}));
                Value s = wsettle(d, w);
                CallResult ex = call(d, "scenario_export", wid(w, {{"name", td::str(name)}, {"overwrite", td::boolean(true)}}));
                check(result_of(s, "status") == "completed" && !ex.is_error,
                      "W5 setup: the fan-out run completes and exports with " + first_step + " first");
                auto sc = td::read_scenario_file(wbase / (name + ".json"));
                return sc ? td::without_field(*sc, "name") : Value{};
            };
            Value const ab = run_ordered("a", "wf_fanout_ab");
            Value const ba = run_ordered("b", "wf_fanout_ba");
            check(!ab.is_null() && agentengine::json::dump(ab) == agentengine::json::dump(ba),
                  "W5: a-first and b-first runs export the same scenario (per-step logs, structural stream, result)");
            auto sc = td::read_scenario_file(wbase / "wf_fanout_ab.json");
            check(sc && td::replay_scenario(*sc).passed, "W5: and it replays");
        }

        // W6: a step that emits more events than the step-event queue holds makes the workflow non-exportable.
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_fanout");
            std::vector<Value> flood;  // 4 turns x 190 echo calls: well past the step-event queue's 1,024
            for (int turn = 0; turn < 4; ++turn) {
                std::vector<std::pair<std::string, std::string>> many;
                for (int i = 0; i < 190; ++i) many.emplace_back("echo", "n" + std::to_string(turn * 190 + i));
                flood.push_back(call_turn(many));
            }
            flood.push_back(text_turn("a done"));
            wpush(d, w, "a", td::arr(std::move(flood)));
            wpush(d, w, "b", td::arr({text_turn("b done")}));
            (void)call(d, "workflow_run", wid(w, {{"text", td::str("flood")}}));
            Value s = wsettle(d, w);
            CallResult ex = call(d, "scenario_export", wid(w, {{"name", td::str("wf_flood")}}));
            check(td::get_u64(s, "events_dropped").value_or(0) > 0 && ex.is_error && ex.error_code == "test.events_dropped",
                  "W6: dropped step events are counted and the workflow cannot be exported (dropped " +
                      std::to_string(td::get_u64(s, "events_dropped").value_or(0)) + ")");
        }

        // W8 + W11: cancel. While suspended: the driver's outcome, exportable, never runnable again.
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_review");
            wpush(d, w, "draft", td::arr({text_turn("draft")}));
            (void)call(d, "workflow_run", wid(w, {{"text", td::str("go")}}));
            (void)wsettle(d, w);
            CallResult c = call(d, "workflow_cancel", wid(w));
            Value s = wsettle(d, w);
            check(!c.is_error && td::get_string(s, "state") == "finished" && td::get_string(s, "driver_outcome") == "cancelled_by_driver",
                  "W8: a cancel while suspended finishes the workflow as cancelled_by_driver");
            CallResult r = call(d, "request_port_resolve", wid(w, {{"interaction_id", td::str(kPort1)}, {"text", td::str("x")}}));
            check(r.is_error && r.error_code == "test.workflow_cancelled", "W8: a cancelled workflow cannot be resolved");
            CallResult ex = call(d, "scenario_export", wid(w, {{"name", td::str("wf_cancel_suspended")}}));
            auto sc = td::read_scenario_file(wbase / "wf_cancel_suspended.json");
            check(!ex.is_error && sc && td::replay_scenario(*sc).passed, "W8: a cancel while suspended exports and replays");
        }
        // While running: non-exportable; run, resolve and push are refused while running (W11).
        {
            struct Gate {
                std::mutex m;
                std::condition_variable cv;
                bool open = false;
            };
            auto gate = std::make_shared<Gate>();
            class GateBackend final : public td::ModelBackend {
            public:
                GateBackend(std::shared_ptr<td::ModelBackend> inner, std::shared_ptr<Gate> g) : inner_(std::move(inner)), g_(std::move(g)) {}
                agentengine::ChatClientCapabilities capabilities() const override { return inner_->capabilities(); }
                agentengine::rt::task<agentengine::result<agentengine::ChatResponse>> chat(agentengine::ChatRequest const& r,
                                                                                          agentengine::EffectContext& ctx) override {
                    {
                        std::unique_lock lock(g_->m);
                        g_->cv.wait_for(lock, std::chrono::seconds(10), [&] { return g_->open; });
                    }
                    co_return co_await inner_->chat(r, ctx);
                }
                agentengine::stream<agentengine::ChatResponseUpdate> chat_stream(agentengine::ChatRequest const& r,
                                                                                 agentengine::EffectContext& ctx) override {
                    return inner_->chat_stream(r, ctx);
                }

            private:
                std::shared_ptr<td::ModelBackend> inner_;
                std::shared_ptr<Gate>             g_;
            };
            td::DriverConfig c = wcfg;
            c.workflow_backend_wrapper = [gate](std::string const&, std::shared_ptr<td::ModelBackend> inner) {
                return std::shared_ptr<td::ModelBackend>(std::make_shared<GateBackend>(std::move(inner), gate));
            };
            auto open_gate = [&] {
                {
                    std::lock_guard lock(gate->m);
                    gate->open = true;
                }
                gate->cv.notify_all();
            };
            {
                td::Driver d(c);
                std::string const w = wstart(d, "wf_review");
                wpush(d, w, "draft", td::arr({text_turn("draft")}));
                (void)call(d, "workflow_run", wid(w, {{"text", td::str("go")}}));
                CallResult push_running = call(d, "workflow_script_push", wid(w, {{"executor_id", td::str("draft")}, {"turns", td::arr({text_turn("x")})}}));
                CallResult list_running = call(d, "request_port_list", wid(w));
                CallResult snap_running = call(d, "workflow_snapshot", wid(w));
                check(push_running.is_error && push_running.error_code == "test.workflow_running" &&
                          list_running.is_error && td::get_bool(snap_running.body, "partial") == true,
                      "W11: while running, script push and port listing are refused and the snapshot is partial");
                CallResult c1 = call(d, "workflow_cancel", wid(w));
                open_gate();
                Value s = wsettle(d, w);
                CallResult ex = call(d, "scenario_export", wid(w, {{"name", td::str("wf_cancel_running")}}));
                check(!c1.is_error && td::get_string(s, "state") == "finished" && ex.is_error && ex.error_code == "test.nondeterministic",
                      "W8: a cancel while running finishes the workflow and makes it non-exportable (status " +
                          result_of(s, "status") + ")");
            }
            // W9: close and driver exit with a run in flight or suspended return and leave nothing running.
            {
                gate->open = false;
                std::thread opener;
                auto t0 = std::chrono::steady_clock::now();
                {
                    td::Driver d(c);
                    std::string const w1 = wstart(d, "wf_review");
                    wpush(d, w1, "draft", td::arr({text_turn("draft")}));
                    (void)call(d, "workflow_run", wid(w1, {{"text", td::str("go")}}));
                    std::string const w2 = wstart(d, "wf_two_ports");
                    (void)call(d, "workflow_run", wid(w2, {{"text", td::str("in")}}));
                    (void)wsettle(d, w2);
                    CallResult closed = call(d, "workflow_close", wid(w2));
                    check(!closed.is_error, "W9: closing a suspended workflow returns");
                    opener = std::thread([&] {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        open_gate();
                    });
                }  // ~Driver with w1 in flight
                opener.join();
                double const secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                check(secs < 15.0, "W9: driver exit with a workflow in flight returns (" + std::to_string(secs) + " s)");
            }
        }

        // W8 caps: at most kMaxWorkflows open.
        {
            td::Driver d(wcfg);
            for (std::size_t i = 0; i < td::kMaxWorkflows; ++i) (void)wstart(d, "wf_two_ports");
            CallResult over = call(d, "workflow_start", td::obj({{"fixture", td::str("wf_two_ports")}}));
            check(over.is_error && over.error_code == "test.too_many_workflows", "W8: at most 4 open workflows");
            CallResult file = call(d, "workflow_start", td::obj({{"fixture", td::str("basic")}}));
            check(file.is_error && file.error_code == "test.unknown_fixture", "W7: a session fixture (or file) is not a workflow fixture");
        }

        // W10: every step's grant is empty.
        {
            td::Driver d(wcfg);
            std::string const w = wstart(d, "wf_fanout");
            wpush(d, w, "a", td::arr({text_turn("a")}));
            wpush(d, w, "b", td::arr({text_turn("b")}));
            (void)call(d, "workflow_run", wid(w, {{"text", td::str("x")}}));
            Value s = wsettle(d, w);
            check(td::get_u64(s, "steps_with_grant") == 2u && td::get_u64(s, "step_grant_kinds") == 0u,
                  "W10: both steps ran under the workflow's grant, and it holds nothing");
        }
        fs::remove_all(wbase, ec);
    }

    // ---- C2 (Windows form): observe from the MCP thread while the worker runs -------------------------------
    {
        td::Driver d;
        std::string const id = start(d, "basic");
        int good = 0;
        // 200 in the default suite; the TSan run (ADR-182 C2) sets AE_C2_ITERATIONS=1000.
        int const kIterations = [] {
            auto const v = agentengine::pal::env_var("AE_C2_ITERATIONS");
            int const n = v ? std::atoi(v->c_str()) : 0;
            return n > 0 ? n : 200;
        }();
        for (int i = 0; i < kIterations; ++i) {
            std::string const expect = "reply " + std::to_string(i);
            push(d, id, td::arr({call_turn({{"echo", "x"}}), text_turn(expect)}));
            (void)call(d, "session_send", sid(id, {{"text", td::str("go")}}));
            for (int k = 0; k < 5; ++k) {
                (void)call(d, "session_snapshot", sid(id));
                (void)call(d, "session_events", sid(id, {{"since_seq", td::num(0)}}));
                (void)call(d, "interaction_list", sid(id));
            }
            Value w = wait(d, id, "idle");
            if (w.find("snapshot") && outcome_field(*w.find("snapshot"), "text") == expect) ++good;
        }
        check(good == kIterations, "C2: " + std::to_string(kIterations) + " runs observed concurrently all settle with their own scripted text (" +
                                       std::to_string(good) + ")");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}
