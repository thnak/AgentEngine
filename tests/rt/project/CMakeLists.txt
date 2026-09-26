# tests/rt/project/: rt project registry/manifest/archive/supervisor (include/agentengine/rt/project_*.hpp).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "rt/project" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- ADR-037 -- agentengine::rt::ProjectRegistry, the Quark-actor-free replacement for the
# in-memory-index half (H1-H3) of 030 Sec6's ProjectRegistry (project/registry.hpp). H4/H5/G4 --
# project/project.hpp's own manifest snapshot and archived-member tail -- are NOT covered here (a
# separate, larger gap needing a real append-only log store primitive, named in
# include/agentengine/rt/project_registry.hpp's own banner) -- test_project_registry.cpp stays in
# place, not deleted, since it is not yet fully superseded.
agentengine_add_test(test_rt_project_registry LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 -- ProjectSupervisor's checkpoint-orchestration role (project/lifecycle.hpp),
# type-erased across heterogeneous rt::AgentSession<...> member instantiations without Quark's
# ActorRef<A>::passivate(). See project_supervisor.hpp's own banner for the "checkpoint, not
# passivate" scope narrowing.
agentengine_add_test(test_rt_project_supervisor LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 -- agentengine::rt::ProjectRegistry's archived-member tail (030 Sec6/§7), wiring
# rt::AppendLogStore into the gap project_registry.hpp's own banner named as deferred: "the archived
# tail is genuinely, structurally append-only... this needs a genuine append-only log store primitive
# nothing in this codebase has built yet." Ports the old test_project_registry.cpp's own H5 (exact
# archival order) and G4 (at-scale, 2000 members) claims functionally; test_project_registry.cpp
# stays in place, not deleted, since its H4/G4 manifest-snapshot-cost claims still have no rt::
# equivalent (no ProjectRecord manifest has been ported yet -- separate, smaller follow-up).
agentengine_add_test(test_rt_project_archive LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 -- agentengine::rt::ProjectRecord's manifest snapshot (rt::SessionStore, single-slot --
# active_members stays small by construction, so no AppendLogStore is needed here unlike the archived
# tail above) and the directed-lifecycle status-flip functions (pause_project/restore_project/
# archive_project), layered on rt::ProjectSupervisor's own checkpoint_members_and_workflows(). Closes
# the last named residual from both rt::ProjectRegistry's and rt::ProjectSupervisor's own banners.
agentengine_add_test(test_rt_project_manifest LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 -- 030 §7's own promotion gate G1 against rt::ProjectSupervisor/rt::pause_project --
# supersedes the old Quark-actor-based test_project_scale_isolation.cpp (and, together with
# test_rt_project_supervisor.cpp's own Q1/Q4, test_project_lifecycle.cpp too -- both retired, see
# their own git history). 100 real AgentSession objects, each with its own ProjectSupervisor and its
# own InMemorySessionStore; pausing #50 is proven to touch ONLY #50's own store (structural
# isolation, standing in for the old Dormant-census/latency measurements that have no rt:: analogue
# -- see this file's own header comment for why), while the other 99 each complete a real second Run
# uninterrupted.
agentengine_add_test(test_rt_project_scale_isolation LIBS agentengine::core agentengine_warnings)
