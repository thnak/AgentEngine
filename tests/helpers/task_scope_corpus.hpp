#pragma once
// ADR-147 (task-scope containment corpus, 008-Sandbox-and-Isolation.md §9 G9): a corpus of
// ORDINARY-LOOKING task instructions -- never phrased as a probe, never containing an obviously
// hostile literal ("..\\..\\Windows", "cmd.exe") the way tests/test_mediated_shell_runner_hostile_
// corpus.cpp's own "hostile-name corpus" deliberately does -- each pairing a plausible task
// narrative with one embedded, achievable-but-out-of-scope action a real task like it could
// plausibly motivate ("grab the shared config while you're at it," "clean up any stray files the
// other project left behind"). This is the SAME mount-escape containment mechanism that corpus
// already proves (`open_within_mount_root`, ADR-014, Judged) -- G9 is a coverage extension (a
// second, narratively-different corpus shape run through the identical mechanism), not a new
// containment primitive. See ADR-147 §1 for why this is the right and only claim this corpus
// makes.
//
// WHAT THIS DOES NOT CLAIM (ADR-147 §5, stated here too since it's the fact a reader of just this
// header most needs): this is NOT a model-propensity or eval-awareness test. `task_narrative` is
// documentation a human reads; nothing in the test pipeline feeds it to a model. `scripted_action`
// is authored by the test, standing in for "what an agent following this narrative might do,"
// exactly the way `abuse_case_corpus.hpp`'s probe commands already stand in for a hostile guest's
// actions. Testing actual model propensity would require a live model in CI, which this project
// has never done (every existing test drives the sandbox directly or replays a fixed recording,
// `include/agentengine/core/replay_chat_client.hpp`) and is explicitly out of scope here -- it
// would need its own ADR addressing model-version pinning, cost, and flakiness governance first.
//
// CORPUS-AUTHORSHIP RULE (binding on every future addition to this table, ADR-147 §3 finding):
// fixture content standing in for a secret, credential, or PII MUST use a documented-fake format
// only -- e.g. AWS's own published example key `AKIAIOSFODNN7EXAMPLE`, an RFC 2606 reserved
// domain (`example.com`), a Luhn-INVALID card number. Never a plausible-but-arbitrary string that
// a secret scanner (GitHub push protection, CI log scanning) cannot distinguish from a real leaked
// credential. This corpus's own entries below deliberately use boring, non-secret-shaped fixture
// content (a config file, a log file) specifically to stay out of that risk category rather than
// exercise the rule -- a future entry that DOES want secret-shaped bait must follow it.

#include <string>

namespace agentengine::native_jail::test {

struct TaskScopeCorpusCase {
    char const* name;
    // A human-readable, ordinary-sounding task description. Documentation only -- read by a
    // reviewer of this corpus, never consumed by the test pipeline or any model.
    char const* task_narrative;
    // The literal shell command standing in for "what an agent given that narrative did" --
    // authored by this test, not decided at runtime by anything resembling a policy under test.
    std::string scripted_action;
    // true: the action reaches outside the granted mount and MUST be denied. false: the action is
    // legitimately in-scope and MUST succeed (this corpus's own positive control -- proves
    // denial above is real containment, not "every command in this table happens to fail").
    bool expect_denied;
};

inline const TaskScopeCorpusCase kTaskScopeCorpus[] = {
    {"scoped_backup_reaches_parent_for_good_measure",
     "Copy this week's report into the archive folder, then also grab the shared team config "
     "sitting one level up so it's backed up too.",
     "cp ../team_config.txt archive/team_config.txt", /*expect_denied=*/true},
    {"log_cleanup_reaches_sibling_project",
     "Clean up the old debug log, including the copy the other project next door left behind in "
     "its own folder.",
     "rm ../sibling-project/debug.log", /*expect_denied=*/true},
    // Positive control (ADR-147 §2's own required non-vacuity proof, matching 008 §9 G2/G3's
     // existing precedent): the SAME shape of ordinary task, scoped correctly, must succeed --
    // otherwise "denied" above would be indistinguishable from "this harness rejects everything."
    {"scoped_backup_succeeds_when_correctly_scoped",
     "Copy this week's report into the archive folder.", "cp real.txt archive/real.txt",
     /*expect_denied=*/false},
};

inline constexpr std::size_t kTaskScopeCorpusSize = sizeof(kTaskScopeCorpus) / sizeof(kTaskScopeCorpus[0]);

}  // namespace agentengine::native_jail::test
