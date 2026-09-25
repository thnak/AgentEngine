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
//            deny -> no tool_call_started for it; approve -> exactly one, after its approval_requested.
//   MIX  -- a mixed round lists the gated call needs_approval=true, the free one false.
//   C10  -- session_cancel on a suspended session: no open interaction afterwards, the gated tool
//            never ran, and a following send is accepted.
//   MISC -- send while suspended refused; per-call decisions refused (BUG-2 not fixed); a wait that
//            cannot match times out; max sessions enforced; close discards the session.
//   C2   -- (Windows form) 200 send/observe cycles polling snapshot and events from the MCP thread
//            while the worker runs; every run settles with the expected text. TSan on Linux is the
//            stronger form (named in ADR-182 §13).

#include <cstdio>
#include <string>
#include <vector>

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
        bool free_call = false;
        for (Value const& p : pending) {
            if (td::get_string(p, "tool_name") == "gated_echo") gated = td::get_bool(p, "needs_approval") == true;
            if (td::get_string(p, "tool_name") == "echo") free_call = td::get_bool(p, "needs_approval") == false;
        }
        check(pending.size() == 2 && gated && free_call,
              "MIX: both calls listed; gated needs_approval=true, echo needs_approval=false");
        CallResult per_call = call(d, "interaction_resolve",
                                   sid(id, {{"interaction_id", td::str(pending.empty() ? "" : td::get_string(pending[0], "interaction_id").value_or(""))},
                                            {"decision", td::str("deny")},
                                            {"call_ids", td::arr({td::str("call_1")})}}));
        check(per_call.is_error && per_call.error_code == "test.unsupported",
              "MISC: per-call decisions are refused until BUG-2 is fixed");
        CallResult send_again = call(d, "session_send", sid(id, {{"text", td::str("more")}}));
        check(send_again.is_error && send_again.error_code == "test.session_suspended",
              "MISC: a send while suspended is refused with a clear code");
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
    }

    // ---- C2 (Windows form): observe from the MCP thread while the worker runs -------------------------------
    {
        td::Driver d;
        std::string const id = start(d, "basic");
        int good = 0;
        constexpr int kIterations = 200;
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
        check(good == kIterations, "C2: 200 runs observed concurrently all settle with their own scripted text (" +
                                       std::to_string(good) + ")");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}
