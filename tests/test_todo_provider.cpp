// Implements decisions/ADR-166-todo-provider.md's proof obligations for `TodoProvider`
// (core/todo_provider.hpp), 005 §5, issue #53. Mirrors test_memory_provider.cpp's structure: each
// block proves one claim from the ADR's per-claim verdict table.

#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/todo_provider.hpp"
#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

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

}  // namespace

int main() {
    namespace json = ae::json;
    ae::Principal const principal{"p-todo", ""};
    std::vector<ae::Message> history{make_msg(ae::role::user, "plan the release", "m-1")};
    ae::EffectContext ctx{};
    ctx.principal = principal;
    ae::SessionContext session_ctx{"s-todo", principal, history};

    ae::TodoProvider provider;

    // --- R1: before any todos_add, on_context() contributes tools but NO instructions/messages ---
    auto out0 = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
        provider.on_context(session_ctx, ctx));
    check(out0.has_value(), "R1: on_context() succeeds before first use");
    check(out0.has_value() && out0->tools.size() == 5,
          "R1: all five tools are always declared (todos_add/complete/remove/get_remaining/get_all), "
          "unconditionally -- the model must see todos_add before it can ever call it");
    check(out0.has_value() && !out0->instructions.has_value() && out0->messages.empty(),
          "R1 (adaptive default, draft §4): an agent that never plans pays nothing -- no guidance "
          "block, no placeholder-empty-list message, unlike MAF's TodoProvider which emits both "
          "unconditionally every turn");

    // Locate each tool by name (declaration order is not part of the contract).
    auto find_tool = [](ae::ContextContribution const& c, std::string_view name) {
        for (auto const& t : c.tools) {
            if (t.name == name) return &t;
        }
        return static_cast<ae::ToolDescriptor const*>(nullptr);
    };
    auto const* add_tool             = find_tool(*out0, "todos_add");
    auto const* complete_tool        = find_tool(*out0, "todos_complete");
    auto const* remove_tool          = find_tool(*out0, "todos_remove");
    auto const* get_remaining_tool   = find_tool(*out0, "todos_get_remaining");
    auto const* get_all_tool         = find_tool(*out0, "todos_get_all");
    check(add_tool && complete_tool && remove_tool && get_remaining_tool && get_all_tool,
          "setup: all five tools are found by name");
    check(get_remaining_tool && get_remaining_tool->effect_class == ae::effect_class::pure &&
              get_all_tool && get_all_tool->effect_class == ae::effect_class::pure,
          "R2 (ADR-166 finding 2): the two read-only tools are effect_class::pure");
    check(add_tool && add_tool->effect_class == ae::effect_class::at_most_once,
          "R2 (ADR-166 finding 2): todos_add keeps ToolDescriptor's conservative default "
          "(at_most_once), NOT effect_class::pure -- it mutates state, so the draft's blanket 'pure' "
          "claim for all five tools was wrong for this one");
    check(add_tool && add_tool->captures_session_state,
          "R3 (ADR-028): todos_add's invoke closure captures `this` (live provider state), so "
          "captures_session_state is stamped true, same convention a persistent interpreter's tools "
          "already use");

    // --- R4: todos_add mutates live state; unknown-id error paths reject cleanly (finding 3) ------
    {
        auto args = json::parse(R"({"title":"write release notes"})");
        check(args.has_value(), "setup: add args parse");
        auto reply_json = add_tool->invoke(*args, ctx);
        check(reply_json.has_value(), "R4: todos_add succeeds");
        auto reply = ae::schema::from_json<ae::TodoAddReply>(*reply_json);
        check(reply.has_value() && reply->id == 0, "R4: the first item gets id 0");

        auto bad_complete = json::parse(R"({"id":999})");
        check(bad_complete.has_value(), "setup: bad id args parse");
        auto complete_result = complete_tool->invoke(*bad_complete, ctx);
        check(!complete_result.has_value() && complete_result.error().code == "todo.unknown_id",
              "R5 (ADR-166 finding 3): todos_complete on an unknown id fails with a real, "
              "distinguishable error code -- never a silent no-op that hides a stale/hallucinated id "
              "from the model");
        auto remove_result = remove_tool->invoke(*bad_complete, ctx);
        check(!remove_result.has_value() && remove_result.error().code == "todo.unknown_id",
              "R5: todos_remove on an unknown id fails the same way");
    }

    // --- R6: on_context() is now adaptive-ON -- instructions + a tainted, provenance-marked status
    // message, exactly the properties MemoryProvider's own retrieved content carries (029 §6) -------
    auto out1 = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
        provider.on_context(session_ctx, ctx));
    check(out1.has_value(), "R6: on_context() still succeeds after first use");
    check(out1.has_value() && out1->instructions.has_value(),
          "R6: instructions become present once the list has been used once, and stay present "
          "(draft §4's 'ever_used_' semantics, not 'list non-empty')");
    check(out1.has_value() && out1->messages.size() == 1 &&
              out1->messages.front().message_id == "todo:status",
          "R6: exactly one status message is contributed");
    check(out1.has_value() && !out1->messages.empty() && !out1->messages.front().content.empty() &&
              out1->messages.front().content.front().tainted &&
              out1->messages.front().content.front().origin == ae::content_origin::external &&
              out1->messages.front().role == ae::role::system,
          "R7 (I3): the status message is role::system + content_origin::external + tainted=true -- "
          "model-echoed content re-presented as data, never re-acquiring authority, matching "
          "MemoryProvider::memory_item_to_message()'s own established precedent");
    check(out1.has_value() && !out1->messages.empty() &&
              std::get<ae::Text>(out1->messages.front().content.front().value).text.find(
                  "write release notes") != std::string::npos,
          "R8: the status message actually renders the added item's title");

    // --- R9: todos_complete flips state; the SAME item shows [x] afterward -------------------------
    {
        auto complete_args = json::parse(R"({"id":0})");
        check(complete_args.has_value(), "setup: complete args parse");
        auto complete_reply = complete_tool->invoke(*complete_args, ctx);
        check(complete_reply.has_value(), "R9: completing a real id succeeds");
        auto ok = ae::schema::from_json<ae::TodoOkReply>(*complete_reply);
        check(ok.has_value() && ok->ok, "R9: the reply reports ok=true");

        auto out2 = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));
        std::string const rendered =
            std::get<ae::Text>(out2->messages.front().content.front().value).text;
        check(rendered.find("[x] write release notes") != std::string::npos,
              "R9: the rendered status reflects completion, not stale [ ]");
    }

    // --- R10: todos_get_remaining / todos_get_all read the SAME live state through their own tools -
    {
        auto add_args = json::parse(R"({"title":"cut the tag"})");
        auto add_reply_json = add_tool->invoke(*add_args, ctx);
        check(add_reply_json.has_value(), "setup: second item added");
        auto add_reply = ae::schema::from_json<ae::TodoAddReply>(*add_reply_json);
        check(add_reply.has_value() && add_reply->id == 1,
              "R11 (finding 4, id monotonicity): the second item gets id 1, never reusing id 0");

        auto no_args = json::parse(R"({})");
        check(no_args.has_value(), "setup: empty-object args parse (TodoNoArgs' one inert field "
                                    "defaults, so an empty object is still a valid payload)");

        auto remaining_json = get_remaining_tool->invoke(*no_args, ctx);
        check(remaining_json.has_value(), "R10: todos_get_remaining succeeds");
        auto remaining = ae::schema::from_json<ae::TodoListReply>(*remaining_json);
        check(remaining.has_value() &&
                  remaining->rendered.find("cut the tag") != std::string::npos &&
                  remaining->rendered.find("write release notes") == std::string::npos,
              "R10: todos_get_remaining excludes the already-completed item and includes the pending "
              "one");

        auto all_json = get_all_tool->invoke(*no_args, ctx);
        check(all_json.has_value(), "R10: todos_get_all succeeds");
        auto all = ae::schema::from_json<ae::TodoListReply>(*all_json);
        check(all.has_value() && all->rendered.find("cut the tag") != std::string::npos &&
                  all->rendered.find("write release notes") != std::string::npos,
              "R10: todos_get_all includes both items regardless of completion state");
    }

    // --- R11b: todos_remove actually removes; id is never reused for a NEW item afterward ---------
    {
        auto remove_args = json::parse(R"({"id":0})");
        auto remove_reply_json = remove_tool->invoke(*remove_args, ctx);
        check(remove_reply_json.has_value(), "setup: removing id 0 succeeds");

        auto add_args = json::parse(R"({"title":"announce"})");
        auto add_reply_json = add_tool->invoke(*add_args, ctx);
        auto add_reply = ae::schema::from_json<ae::TodoAddReply>(*add_reply_json);
        check(add_reply.has_value() && add_reply->id == 2,
              "R11b: after removing id 0, the NEXT new item still gets id 2 (next_id_ is monotonic, "
              "never rewound by a removal) -- a removed id can never collide with a later item");
    }

    // --- R12: the adaptive contribution stays ON even if every item is later cleared ---------------
    {
        auto remove1 = json::parse(R"({"id":1})");
        auto remove2 = json::parse(R"({"id":2})");
        check(remove_tool->invoke(*remove1, ctx).has_value(), "setup: remove id 1");
        check(remove_tool->invoke(*remove2, ctx).has_value(), "setup: remove id 2");

        auto out3 = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));
        check(out3.has_value() && out3->instructions.has_value() && out3->messages.size() == 1,
              "R12 (draft §4's stated intent): once engaged, guidance stays visible even after the "
              "list empties back out -- keyed on 'ever used', not 'list non-empty'");
        std::string const rendered =
            std::get<ae::Text>(out3->messages.front().content.front().value).text;
        check(rendered.find("none yet") != std::string::npos,
              "R12: an emptied list renders the same 'none yet' placeholder MAF's own message uses");
    }

    // --- R13: DoS bounds (ADR-166 finding 1) are enforced by REJECTION, not truncation -------------
    {
        ae::TodoProvider fresh;
        auto ctx2 = ctx;

        std::string const long_title(ae::TodoProvider::kMaxTitleLength + 1, 'x');
        auto long_args = json::parse(R"({"title":")" + long_title + R"("})");
        check(long_args.has_value(), "setup: long-title args parse");
        auto out_ctx = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            fresh.on_context(session_ctx, ctx2));
        auto const* fresh_add = find_tool(*out_ctx, "todos_add");
        check(fresh_add != nullptr, "setup: fresh provider's todos_add found");
        auto long_result = fresh_add->invoke(*long_args, ctx2);
        check(!long_result.has_value() && long_result.error().code == "todo.title_too_long",
              "R13a: a title over kMaxTitleLength is REJECTED, not silently truncated");

        auto empty_args = json::parse(R"({"title":""})");
        auto empty_result = fresh_add->invoke(*empty_args, ctx2);
        check(!empty_result.has_value() && empty_result.error().code == "todo.empty_title",
              "R13b: an empty title is rejected");

        for (std::size_t i = 0; i < ae::TodoProvider::kMaxItems; ++i) {
            auto ok_args = json::parse(R"({"title":"item"})");
            auto ok_result = fresh_add->invoke(*ok_args, ctx2);
            check(ok_result.has_value(), "R13c setup: filling the list up to kMaxItems succeeds");
        }
        auto overflow_args = json::parse(R"({"title":"one too many"})");
        auto overflow_result = fresh_add->invoke(*overflow_args, ctx2);
        check(!overflow_result.has_value() && overflow_result.error().code == "todo.list_full",
              "R13c: the (kMaxItems + 1)th add is rejected with todo.list_full, not silently dropped "
              "and not allowed to grow unbounded");
    }

    // --- R14: composes with another real ContextProvider through the SAME generic assemble_context()
    // MemoryProvider already proves this against (Phase B3's deferred multi-contributor generalization)
    {
        ae::TodoProvider composed_provider;
        std::vector<ae::ContextProviderDescriptor> contributors;
        contributors.push_back(ae::make_context_provider_descriptor(ae::HistoryProvider<ae::Window<0>>{},
                                                                       ae::ContextBudget{0}));
        contributors.push_back(
            ae::make_context_provider_descriptor(std::move(composed_provider), ae::ContextBudget{0}));

        std::vector<ae::Message> combined_history{make_msg(ae::role::user, "ship it", "m-2")};
        ae::SessionContext combined_ctx{"s-todo", principal, combined_history};
        ae::EffectContext combined_effect_ctx{};
        combined_effect_ctx.principal = principal;

        auto assembled_result = ae::test_support::run_task_sync<ae::result<ae::ContextAssemblyResult>>(
            ae::assemble_context(contributors, combined_ctx, combined_effect_ctx));
        check(assembled_result.has_value(), "R14: assemble_context() succeeds with TodoProvider as a "
                                             "second real contributor");
        check(assembled_result.has_value() &&
                  assembled_result->combined.tools.size() == 5 &&
                  assembled_result->combined.tools.front().name == "todos_add",
              "R14: TodoProvider's five tools survive assembly into the combined tool table");
    }

    std::printf("test_todo_provider: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
