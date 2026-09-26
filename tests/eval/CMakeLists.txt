# tests/eval/: the evaluation harness and approved-lesson delivery (include/agentengine/eval/, ADR-191/192/195).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "eval" ctest label for every test here (cmake/AgentEngineTests.cmake).

if(WIN32)
  # ADR-195 round 5: LessonCandidate/render_lesson pulled forward from ADR-179 stage 3 (§3.0 item 1,
  # E26) as real, pure host code, plus the ADR-179 §3.3 / ADR-195 §3.7 candidate-value validator.
  agentengine_add_test(test_lesson_candidate
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-195 round-4/5 E25 fix: eval: tenant minting refuses the colon-collision round 4 executed
  # against real memory_mount_id/memory_ref_name.
  agentengine_add_test(test_eval_principal
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-195 round-4/5 E31 fix: the approval ack is bound to a digest of the verbatim rendered item,
  # and promotion refuses to write on any mismatch (value, salience, or a stale/placeholder digest).
  agentengine_add_test(test_promotion_ack
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-195 round 5: the Tier-1 statistics (E27/E28, §3.7 gate rule 2, and round 4's §6 G3
  # concentration fix) as real code, pinned against closed-form values and the Python prototype.
  agentengine_add_test(test_tier1_statistics
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-195 trial-running harness, first slice: EvalStore (§3.9) -- a fresh in-memory object/ref
  # store pair, an eval-tenant Principal, and mount-scoped read/write capabilities, proven isolated
  # by the fresh store object rather than by name.
  agentengine_add_test(test_eval_store
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-195 trial-running harness, first slice: host-authored stub ToolDescriptors (§3.9) that
  # record arguments and cause no effect, plus EvalStubToolProvider, the trial's third composed
  # context provider (the only way to add tools to a trial's ToolTable).
  agentengine_add_test(test_eval_stub_tool
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-195 trial-running harness, first slice: run_trial (§3.0 items 2-4, §3.2, §3.4) -- the first
  # real end-to-end driver, proving both delivery routes against a real AgentSession.
  agentengine_add_test(test_eval_trial_driver
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-195 §3.0 item 2, follow-rate screen: the first slice of multi-trial orchestration -- runs
  # N+N real run_trial calls (seeded arm interleaving), grades each structurally (§3.5), and wires
  # the aggregate counts into tier1_statistics.hpp's follow_rate_screen_passes/
  # clopper_pearson_lower_bound for real.
  agentengine_add_test(test_eval_follow_rate_screen
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-195 §3.0 item 3, gross-harm regression screen: 2*K*tasks real run_trial calls (arms and tasks
  # interleaved by one seeded shuffle), each graded for task success, wired into
  # tier1_statistics.hpp's sign-flip sum and hypergeometric min-task statistics for real.
  add_executable(test_eval_gross_harm_screen test_eval_gross_harm_screen.cpp)
  target_link_libraries(test_eval_gross_harm_screen PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  # The MSVC ASan build crossed the object-file section limit (C1128) once PR #100's scenarios S14-S16
  # added more coroutine/lambda instantiations: a mechanical compiler limit, not a code defect -- the same
  # documented, scoped-to-one-target /bigobj as agentengine_cli_chat in the root CMakeLists.txt.
  if(MSVC)
    target_compile_options(test_eval_gross_harm_screen PRIVATE /bigobj)
  endif()
  add_test(NAME test_eval_gross_harm_screen COMMAND test_eval_gross_harm_screen)

  # ADR-195 §3.0 "Tier-1 pre-registration and attempt accounting" (E32): the pre-registration digest is
  # recorded before any trial runs, every started attempt for a lesson family is counted in an append-only
  # log, and the Tier1ScreenResult shows every attempt's figures, not only the passing one.
  add_executable(test_eval_tier1_screen test_eval_tier1_screen.cpp)
  target_link_libraries(test_eval_tier1_screen PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  if(MSVC)
    target_compile_options(test_eval_tier1_screen PRIVATE /bigobj)
  endif()
  add_test(NAME test_eval_tier1_screen COMMAND test_eval_tier1_screen)
endif()

if(AGENTENGINE_WITH_HTTPS)
  # ADR-191: an approved lesson stays tainted and fenced; the session is the one place its approval is granted,
  # after re-verifying the exact text; both serializers neutralize every fence marker they did not emit.
  add_executable(test_approved_lesson_delivery test_approved_lesson_delivery.cpp)
  target_link_libraries(test_approved_lesson_delivery PRIVATE
    agentengine::core agentengine::provider_http_client agentengine::worktree_store agentengine_warnings)
  if(MSVC)
    target_compile_options(test_approved_lesson_delivery PRIVATE /bigobj)
  endif()
  add_test(NAME test_approved_lesson_delivery COMMAND test_approved_lesson_delivery)

  # ADR-192: unattended mode, the delivery half -- approved lessons as instructions, the system-channel fence off,
  # automatic approvals. The approval half is test_unattended_approvals (no HTTPS needed).
  add_executable(test_unattended_mode test_unattended_mode.cpp)
  target_link_libraries(test_unattended_mode PRIVATE
    agentengine::core agentengine::provider_http_client agentengine::worktree_store agentengine_warnings)
  if(MSVC)
    target_compile_options(test_unattended_mode PRIVATE /bigobj)
  endif()
  add_test(NAME test_unattended_mode COMMAND test_unattended_mode)

  # ADR-191: a multi-turn chat with a real agent, driven by ANOTHER agent playing the user through a file mailbox
  # (--serve / --say / --analyze). Not a ctest test -- it needs a second agent to talk to it.
  add_executable(lesson_chat_live lesson_chat_live.cpp)
  target_link_libraries(lesson_chat_live PRIVATE
    agentengine::core agentengine::provider_http_client agentengine::worktree_store agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  if(MSVC)
    target_compile_options(lesson_chat_live PRIVATE /bigobj)
  endif()

  # ADR-195 trial-running harness, first slice: one real B trial and one real T trial through
  # run_trial against a real model -- the end-to-end sanity check the scripted-client driver test
  # cannot provide on its own. Live, observation only; self-skips without a key.
  agentengine_add_test(test_eval_trial_driver_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine::worktree_store
      agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_eval_trial_driver_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ADR-195 PR #100: the summarizer's recording and token budget against a REAL summarizer model
  # (every other test drives the summarizer with a scripted mock).
  agentengine_add_test(test_eval_summarizer_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine::worktree_store
      agentengine_warnings MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_eval_summarizer_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)

  # ADR-195 E32 against a real model, with HARD goals: three counted Tier-1 attempts (full screens, the ADR's
  # default sizes) log every model turn to a file as it happens, and an analysis that reads only the files --
  # that action log and the on-disk attempt log -- decides pass/fail. Artifacts are kept for a human.
  # Self-skips without a key; `--analyze <dir>` re-runs the analysis on a kept run.
  add_executable(test_eval_tier1_screen_live_e2e test_eval_tier1_screen_live_e2e.cpp)
  target_link_libraries(test_eval_tier1_screen_live_e2e PRIVATE
    agentengine::core agentengine::provider_http_client agentengine::worktree_store agentengine_warnings
    MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  if(MSVC)
    target_compile_options(test_eval_tier1_screen_live_e2e PRIVATE /bigobj)
  endif()
  add_test(NAME test_eval_tier1_screen_live_e2e COMMAND test_eval_tier1_screen_live_e2e)
  set_tests_properties(test_eval_tier1_screen_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 7200)
endif()
