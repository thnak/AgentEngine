// Proof for ADR-055's own §6 residual, now closed: real merge-on-join wiring, bridging
// core/worktree.hpp's already-Judged merge machinery, workflow/worktree_scoping.hpp's `mint_executor_
// worktrees()`/new `make_merge_on_join_hook()`, and rt::WorkflowSupervisor's new `MergeOnJoinHook`.
// Confirmed by direct investigation before this file existed: nothing in this codebase previously
// drove a branch-mode executor through WorkflowSupervisor to completion at all -- the worktree-scoping
// and workflow-execution subsystems were proven in complete isolation from each other. This file is
// the first to combine them.
//
//   J1 -- happy path: a single branch-mode executor writes a new file into its OWN worktree; the
//         SUPERVISOR ITSELF folds it back into the parent via the merge-on-join hook -- proven by
//         re-reading the parent ref directly afterward, with NO manual merge_branch_into_parent call
//         anywhere in this test.
//   J2 -- conflict path: a branch-mode executor and a shared-mode executor, run in the SAME round,
//         both edit the SAME file differently -- the shared writer's edit lands directly on the
//         parent (shared mode has no merge step); the branch's own merge-on-join then genuinely
//         conflicts against that already-moved parent. The run terminates with
//         workflow_status::merge_conflict, `failed_executor` names the branch, the PARENT keeps the
//         shared writer's own edit untouched (025 §4: never last-writer-wins), and real conflict
//         evidence is durably materialized at conflicts_ref_name(...) -- both versions retained.
//   J3 -- disjoint changes across two independent branch executors in the same round both merge
//         cleanly -- proving the hook handles more than one branch-mode completion per round.

#include <cstdio>
#include <string>

#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/workflow/worktree_scoping.hpp"

using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;

using namespace agentengine;
using namespace agentengine::workflow;
using InMemoryStore = agentengine::rt::InMemoryAppendLogStore;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

std::vector<std::byte> bytes_of(std::string const& content) {
    std::vector<std::byte> bytes;
    bytes.reserve(content.size());
    for (char c : content) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return bytes;
}

std::string string_of(std::vector<std::byte> const& bytes) {
    std::string s;
    s.reserve(bytes.size());
    for (auto b : bytes) s.push_back(static_cast<char>(b));
    return s;
}

Digest blob_of(InMemoryWorktreeObjectStore& store, std::string const& content) {
    return *store.put_blob(bytes_of(content));
}

Digest tree_of(InMemoryWorktreeObjectStore& store, std::vector<TreeEntry> entries) {
    return *store.put_tree(Tree{std::move(entries)});
}

Executor make_executor(std::string id, sharing_mode mode) {
    Executor ex;
    ex.id            = std::move(id);
    ex.kind          = executor_kind::function;
    ex.input_type    = "T";
    ex.output_type   = "T";
    ex.worktree_mode = mode;
    return ex;
}

// A branch/shared executor's own ExecutorBody: reads its CURRENT worktree tree, adds (or replaces)
// one entry, writes the new tree back through its own grant. Real worktree I/O, not a simulation.
ExecutorBody writer_body(InMemoryWorktreeObjectStore& obj_store, InMemoryStore& ref_store,
                          ExecutorWorktreeGrant grant, std::string file_name, std::string content) {
    return [&obj_store, &ref_store, grant, file_name, content](
               Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        auto current = read_sub_worktree(ref_store, grant.sub);
        if (!current || !current->has_value()) {
            return std::unexpected(
                error{failure_class::fatal, "writer_body: no current tree", "test.setup_failed"});
        }
        auto current_tree = obj_store.get_tree((*current)->tree_digest);
        if (!current_tree) return std::unexpected(current_tree.error());

        std::vector<TreeEntry> entries;
        for (auto const& e : current_tree->entries) {
            if (e.name != file_name) entries.push_back(e);
        }
        entries.push_back(TreeEntry{file_name, blob_of(obj_store, content), false});
        auto new_tree_digest = tree_of(obj_store, entries);

        auto written = write_sub_worktree(ref_store, grant.sub, new_tree_digest);
        if (!written) return std::unexpected(written.error());
        return ExecutorOutcome{in};
    };
}

ExecutorBody passthrough_body() {
    return [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        return ExecutorOutcome{in};
    };
}

}  // namespace

int main() {
    // --- J1: happy path -- the supervisor itself folds a branch's write back into the parent -------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root   = tree_of(obj_store, {{"shared.txt", blob_of(obj_store, "shared-content"), false}});
        auto parent = commit_ref(ref_store, "wf:run-j1", root);
        check(parent.has_value(), "J1 setup: parent commit succeeds");

        Workflow wf;
        wf.id                = "merge-j1";
        wf.executors          = {make_executor("writer", sharing_mode::branch)};
        wf.start              = "writer";
        wf.output_selection.push_back("writer");
        wf.bound.max_rounds   = 4;
        check(validate_workflow(wf).has_value(), "J1 setup: the graph validates");

        auto grants = mint_executor_worktrees(ref_store, *parent, wf);
        check(grants.has_value() && grants->size() == 1, "J1 setup: one grant minted");

        std::vector<ExecutorBody> bodies = {
            writer_body(obj_store, ref_store, (*grants)[0], "new.txt", "new-content")};

        auto hook = make_merge_on_join_hook(obj_store, ref_store, parent->name, "workflow", wf, *grants);

        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        sup.set_merge_on_join_hook(hook);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
        check(r.status == workflow_status::completed, "J1: the run completes normally");

        auto parent_after = read_ref(ref_store, "wf:run-j1");
        check(parent_after.has_value() && parent_after->has_value(), "J1: the parent ref still exists");
        if (parent_after.has_value() && parent_after->has_value()) {
            auto parent_tree = obj_store.get_tree((*parent_after)->tree_digest);
            check(parent_tree.has_value() && parent_tree->entries.size() == 2,
                  "J1: the parent's tree now has BOTH shared.txt and new.txt -- the branch's own "
                  "write folded back, driven entirely by WorkflowSupervisor's own merge-on-join hook, "
                  "with NO manual merge_branch_into_parent call anywhere in this test");
        }
    }

    // --- J2: conflict path -- a shared writer moves the parent; the branch's own merge then --------
    // --- genuinely conflicts against it, in the SAME round.                                       ---
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root   = tree_of(obj_store, {{"a.txt", blob_of(obj_store, "v1"), false}});
        auto parent = commit_ref(ref_store, "wf:run-j2", root);
        check(parent.has_value(), "J2 setup: parent commit succeeds");

        Workflow wf;
        wf.id        = "merge-j2";
        wf.executors = {make_executor("start", sharing_mode::shared),
                        make_executor("branch_writer", sharing_mode::branch),
                        make_executor("shared_writer", sharing_mode::shared)};
        wf.edges.push_back(Edge{"start", "branch_writer", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "shared_writer", edge_kind::fan_out, {}});
        wf.start = "start";
        wf.output_selection.push_back("branch_writer");
        wf.bound.max_rounds = 4;
        check(validate_workflow(wf).has_value(), "J2 setup: the graph validates");

        auto grants = mint_executor_worktrees(ref_store, *parent, wf);
        check(grants.has_value() && grants->size() == 3, "J2 setup: three grants minted");

        std::vector<ExecutorBody> bodies = {
            passthrough_body(),
            writer_body(obj_store, ref_store, (*grants)[1], "a.txt", "branch-edit"),
            writer_body(obj_store, ref_store, (*grants)[2], "a.txt", "shared-edit"),
        };

        auto hook = make_merge_on_join_hook(obj_store, ref_store, parent->name, "workflow", wf, *grants);

        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        sup.set_merge_on_join_hook(hook);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
        check(r.status == workflow_status::merge_conflict,
              "J2: the run terminates with the real merge_conflict status, not completed/executor_failed");
        check(r.failed_executor == "branch_writer",
              "J2: failed_executor names the branch executor whose merge actually conflicted");

        auto parent_after = read_ref(ref_store, "wf:run-j2");
        check(parent_after.has_value() && parent_after->has_value(), "J2: the parent ref still exists");
        if (parent_after.has_value() && parent_after->has_value()) {
            auto parent_tree = obj_store.get_tree((*parent_after)->tree_digest);
            auto blob_content = obj_store.get_blob(parent_tree->entries[0].digest);
            check(parent_tree.has_value() && parent_tree->entries.size() == 1 && blob_content.has_value() &&
                      string_of(*blob_content) == "shared-edit",
                  "J2: the parent keeps the SHARED writer's own edit, completely untouched by the "
                  "rejected branch merge -- 025 §4's own never-last-writer-wins guarantee");
        }

        auto conflicts_ref = read_ref(ref_store, conflicts_ref_name("wf:run-j2"));
        check(conflicts_ref.has_value() && conflicts_ref->has_value(),
              "J2: real conflict evidence was durably materialized (ADR-055) -- both versions "
              "retained, not merely a returned error nobody durably recorded");
        if (conflicts_ref.has_value() && conflicts_ref->has_value()) {
            auto tree = obj_store.get_tree((*conflicts_ref)->tree_digest);
            check(tree.has_value() && tree->entries.size() == 2,
                  "J2: both ours (the parent/shared side) and theirs (the branch side) are present "
                  "in the materialized conflict evidence");
        }
    }

    // --- J3: two independent branch executors in the same round both merge cleanly -----------------
    {
        InMemoryStore ref_store;
        InMemoryWorktreeObjectStore obj_store;
        auto root   = tree_of(obj_store, {{"base.txt", blob_of(obj_store, "base"), false}});
        auto parent = commit_ref(ref_store, "wf:run-j3", root);
        check(parent.has_value(), "J3 setup: parent commit succeeds");

        Workflow wf;
        wf.id        = "merge-j3";
        wf.executors = {make_executor("start", sharing_mode::shared),
                        make_executor("branch_1", sharing_mode::branch),
                        make_executor("branch_2", sharing_mode::branch)};
        wf.edges.push_back(Edge{"start", "branch_1", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "branch_2", edge_kind::fan_out, {}});
        wf.start = "start";
        wf.output_selection.push_back("branch_1");
        wf.bound.max_rounds = 4;
        check(validate_workflow(wf).has_value(), "J3 setup: the graph validates");

        auto grants = mint_executor_worktrees(ref_store, *parent, wf);
        check(grants.has_value() && grants->size() == 3, "J3 setup: three grants minted");

        std::vector<ExecutorBody> bodies = {
            passthrough_body(),
            writer_body(obj_store, ref_store, (*grants)[1], "one.txt", "one-content"),
            writer_body(obj_store, ref_store, (*grants)[2], "two.txt", "two-content"),
        };

        auto hook = make_merge_on_join_hook(obj_store, ref_store, parent->name, "workflow", wf, *grants);

        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        sup.set_merge_on_join_hook(hook);

        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("go")}));
        check(r.status == workflow_status::completed, "J3: the run completes normally");

        auto parent_after = read_ref(ref_store, "wf:run-j3");
        if (parent_after.has_value() && parent_after->has_value()) {
            auto parent_tree = obj_store.get_tree((*parent_after)->tree_digest);
            check(parent_tree.has_value() && parent_tree->entries.size() == 3,
                  "J3: the parent's tree has base.txt PLUS both branches' own disjoint additions -- "
                  "two independent branch-executor completions in one round both merged, in sequence, "
                  "through the same hook");
        }
    }

    if (g_failures == 0) {
        std::printf("test_rt_workflow_supervisor_merge_on_join: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_workflow_supervisor_merge_on_join: %d failure(s)\n", g_failures);
    return 1;
}
