# tests/rt/agent_spawn/: agent.spawn: nested runs, spawn budgets, child capabilities and worktrees, delegation provenance.
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "rt/agent_spawn" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- agent.spawn depth/budget bound (decisions/ADR-006-agent-spawn-depth-budget-bound.md) -------
# Header-only (trust/spawn_budget.hpp), core-tier (std only, no crypto -- in-process, never crosses
# a process boundary the way trust/capability_token.hpp's ADR-005 token does) -- links only
# agentengine::core.
agentengine_add_test(test_spawn_budget LIBS agentengine::core agentengine_warnings)

# ---- agent.spawn budget wiring + child capability minting (items 4/5,
# docs/planning/agent-spawn-runtime-design-draft.md; OpenQuestions.md OQ-14) ----------------------
# Header-only (trust/agent_spawn_capability.hpp), core-tier -- links agentengine::core (which also
# carries rt/spawn_cost_budget.hpp, ADR-031/ADR-037's AsyncMutex-serialized token pool this test
# drives directly alongside trust::SpawnBudget/ADR-006).
agentengine_add_test(test_agent_spawn_capability LIBS agentengine::core agentengine_warnings)

# ---- agent.spawn's nested-run invocation mechanism (item 2, docs/planning/
# agent-spawn-runtime-design-draft.md §4.2; OpenQuestions.md OQ-14) ------------------------------
# Header-only (rt/agent_spawn_child_run.hpp), rt-tier -- links agentengine::core (item 4/5's already-
# landed trust/agent_spawn_capability.hpp) and rt::AgentSession (agent_session.hpp) directly.
agentengine_add_test(test_rt_agent_spawn_child_run LIBS agentengine::core agentengine_warnings)

# ---- agent.spawn's dynamic sub-worktree minting (item 3, docs/planning/
# agent-spawn-runtime-design-draft.md §4.3; OpenQuestions.md OQ-14) ------------------------------
# core/agent_spawn_worktree.hpp calls compute_digest() (derive_spawn_child_id) -- same Windows-only
# real-crypto dependency every other worktree test above (test_workflow_worktree_scoping et al.)
# already needs; also links item 2 (agentengine::core carries rt/agent_spawn_child_run.hpp) for T11's
# end-to-end wiring proof.
if(WIN32)
  agentengine_add_test(test_agent_spawn_worktree
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# ---- agent.spawn's REAL, wired, end-to-end call path (item 1, docs/planning/
# agent-spawn-runtime-design-draft.md §2/§4.1; OpenQuestions.md OQ-14) ---------------------------
# Header-only (rt/agent_spawn.hpp), rt-tier -- composes items 2/3/4/5 above through the REAL
# Tool<>-conforming AgentSpawnTool/AgentSpawnToolProvider/perform_agent_spawn() surface. Same
# WIN32-only compute_digest dependency as test_agent_spawn_worktree (core/agent_spawn_worktree.hpp's
# derive_spawn_child_id() is reached via SpawnPump::process(), not called directly by this file, but
# still links against it).
if(WIN32)
  agentengine_add_test(test_rt_agent_spawn
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # ADR-193: a hop never changes who wrote the text -- A -> B -> C through the real agent.spawn path, plus a
  # workflow agent node. Same WIN32-only compute_digest dependency as test_rt_agent_spawn.
  add_executable(test_delegation_provenance test_delegation_provenance.cpp)
  target_link_libraries(test_delegation_provenance PRIVATE
    agentengine::core agentengine::worktree_store agentengine_warnings)
  if(MSVC)
    target_compile_options(test_delegation_provenance PRIVATE /bigobj)
  endif()
  add_test(NAME test_delegation_provenance COMMAND test_delegation_provenance)
endif()

# ---- ADR-037 -- agentengine::rt::SpawnCostBudget, the Quark-actor-free replacement for
# 026 §9 Q1 / OQ-14's SpawnCostBudgetActor (rt::AsyncMutex replaces quark::Actor<Sequential> as the
# check-and-decrement serialization mechanism).
agentengine_add_test(test_rt_spawn_cost_budget LIBS agentengine::core agentengine_warnings)

# ---- ADR-079 §7's TSan residual on SpawnPump (rt/agent_spawn.hpp) -- a portable, test-local
# reproduction of SpawnPump's own cost-consumption synchronization shape (rt::SpawnCostBudget only,
# no worktree/compute_digest dependency), so it builds and links on every platform including Linux,
# where the real SpawnPump (test_rt_agent_spawn.cpp, below, WIN32-only) cannot run under TSan.
agentengine_add_test(test_rt_spawn_pump_concurrency LIBS agentengine::core agentengine_warnings)
