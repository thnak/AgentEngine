// Implements 014-Workflow-and-Orchestration.md §5's time-travel bullet (rewind to any retained
// checkpoint, optionally with modified state, every rewind audited) and the milestone-6 breakdown
// doc's decision 9 (the audit obligation built WITH the rewind, in the same phase). Milestone 6
// Phase F (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// Pure Store-level tests, no actor involved -- mirrors test_effect_journal.cpp's own precedent
// (checkpoint.hpp's own free functions are what actually touch a `WorkflowSupervisor`; rewind
// selection and audit are a layer below that, provable against a bare `quark::InMemoryStore`).

#include <cstdio>
#include <string>

#include "quark/core/persistence.hpp"

#include "agentengine/core/content_record.hpp"
#include "agentengine/workflow/checkpoint.hpp"
#include "agentengine/workflow/time_travel.hpp"

using namespace quark;
using namespace agentengine;
using namespace agentengine::workflow;

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

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.value = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] RunStateRecord record_at(std::string run_id, std::uint32_t rounds, std::string marker) {
    RunStateRecord rec;
    rec.run_id           = std::move(run_id);
    rec.rounds           = rounds;
    rec.selected_output  = to_record(text_message(std::move(marker)));
    return rec;
}

}  // namespace

int main() {
    InMemoryStore     store;
    std::string const run_id = "tt-run-1";
    auto const        id     = workflow_run_actor_id(run_id);
    auto const        fence  = store.acquire_fence(id);
    auto const        audit_id    = workflow_rewind_audit_actor_id(run_id);
    auto const        audit_fence = store.acquire_fence(audit_id);

    RunStateRecord const rec1 = record_at(run_id, 1, "r1");
    RunStateRecord const rec2 = record_at(run_id, 2, "r2");
    RunStateRecord const rec3 = record_at(run_id, 3, "r3");
    check(save_workflow_checkpoint(store, run_id, fence, 1, rec1).has_value(), "setup: checkpoint 1 saved");
    check(save_workflow_checkpoint(store, run_id, fence, 2, rec2).has_value(), "setup: checkpoint 2 saved");
    check(save_workflow_checkpoint(store, run_id, fence, 3, rec3).has_value(), "setup: checkpoint 3 saved");

    // --- TT1: rewinding to an index with no retained checkpoint fails closed --------------------
    auto missing = rewind_workflow(store, run_id, audit_fence, /*to=*/99, "op-1", "probe");
    check(!missing.has_value() && missing.error().code == quark::errc::not_found,
          "TT1: rewinding to a never-retained checkpoint index fails closed, named");

    // --- TT2: a bare rewind (no modified state) returns the retained record verbatim ------------
    auto r1 = rewind_workflow(store, run_id, audit_fence, /*to=*/2, "op-1", "check the round-2 state");
    check(r1.has_value() && *r1 == rec2,
          "TT2: rewinding to checkpoint 2 returns exactly checkpoint 2's own retained record");

    auto audit1 = read_rewind_audit_log(store, run_id);
    check(audit1.has_value() && audit1->size() == 1, "TT2: one audited rewind so far");
    if (audit1.has_value() && audit1->size() == 1) {
        auto const& a = (*audit1)[0];
        check(a.run_id == run_id && a.from_checkpoint_index == 3 && a.to_checkpoint_index == 2 &&
                  a.operator_id == "op-1" && a.reason == "check the round-2 state" && !a.state_modified,
              "TT2 (014 §5 'every rewind is audited'): the audit entry names WHO (operator_id), "
              "WHERE FROM (the latest retained checkpoint, 3) and WHERE TO (2), and that this was a "
              "bare rewind, not a modified-state one");
    }

    // --- TT3: rewinding this run's CHECKPOINT log is untouched by the rewind call itself ---------
    // rewind_workflow only READS retained_checkpoints and WRITES to the separate audit log -- it
    // must not, itself, mutate what "the latest checkpoint" means for an ordinary crash-recovery
    // resume (checkpoint.hpp's own load_workflow_checkpoint).
    auto still_latest = load_workflow_checkpoint(store, run_id);
    check(still_latest.has_value() && still_latest->has_value() && **still_latest == rec3,
          "TT3: after rewinding (reading, not applying anything), load_workflow_checkpoint still "
          "reports checkpoint 3 as latest -- rewind_workflow does not itself alter the checkpoint log");

    // --- TT4: rewinding WITH modified state returns the caller's override, not the retained one --
    RunStateRecord modified = rec2;
    modified.rounds          = 2;
    modified.selected_output = to_record(text_message("r2-but-edited"));
    auto r2 = rewind_workflow(store, run_id, audit_fence, /*to=*/2, "op-2", "patch the output",
                              modified);
    check(r2.has_value() && *r2 == modified && !(*r2 == rec2),
          "TT4 (014 §5 'optionally with modified state'): the returned record is the CALLER's "
          "override, not silently the retained checkpoint 2 the caller started from");

    auto audit2 = read_rewind_audit_log(store, run_id);
    check(audit2.has_value() && audit2->size() == 2, "TT4: a second audited rewind, appended, not replacing the first");
    if (audit2.has_value() && audit2->size() == 2) {
        check((*audit2)[1].state_modified && (*audit2)[1].to_checkpoint_index == 2 &&
                  (*audit2)[1].operator_id == "op-2",
              "TT4: the second audit entry correctly records state_modified=true and its own "
              "operator/target, independent of the first entry");
        check((*audit2)[0] == (*audit1)[0],
              "TT4: the FIRST audit entry is unchanged by the second rewind -- an append-only log, "
              "not an overwritten single record");
    }

    // --- TT5: read_rewind_audit_log on a run that was never rewound is empty, not an error --------
    auto never = read_rewind_audit_log(store, "tt-run-untouched");
    check(never.has_value() && never->empty(),
          "TT5: a run with no rewind history reads back an empty audit log, not an error");

    if (g_failures == 0) {
        std::printf("test_workflow_time_travel: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_workflow_time_travel: %d failure(s)\n", g_failures);
    return 1;
}
