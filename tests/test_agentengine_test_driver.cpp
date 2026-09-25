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
//   C2   -- (Windows form) 200 send/observe cycles polling snapshot and events from the MCP thread
//            while the worker runs; every run settles with the expected text. TSan on Linux is the
//            stronger form (named in ADR-182 §13).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/pal/env.hpp"
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

        td::Driver no_root;
        std::string const id3 = start(no_root, "basic");
        CallResult dis = call(no_root, "scenario_export", sid(id3, {{"name", td::str("x")}}));
        check(dis.is_error && dis.error_code == "test.export_disabled", "SCN: export is disabled without a scenarios root");
        fs::remove_all(root, ec);
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
