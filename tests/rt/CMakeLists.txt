# tests/rt/: the agentengine::rt runtime substrate (include/agentengine/rt/): tasks, block_on, channels, thread pool, append-log/session stores, effect journal, multi-agent.
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "rt" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- decisions/ADR-066-*.md §7 residual: Message::attribution now survives a round-trip through
# rt/message_codec.hpp's JSON codec (previously confirmed absent).
agentengine_add_test(test_message_codec_attribution LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 -- agentengine::rt::EffectJournalEntry (019 §3's effect journaling: intent before
# execution, outcome after), ported off the old Quark quark::Store/EventLog shape onto
# rt::AppendLogStore -- name-scoped by session_id rather than sharing a literal Store/ActorId slot
# with the session's own checkpoint (rt::SessionStore and rt::AppendLogStore are structurally
# different interfaces; see effect_journal.hpp's own banner). Supersedes test_effect_journal.cpp,
# whose F2-R1..R7 claims are ported (J1-J7) and whose F2-R8 is replaced with a genuinely analogous
# J8 (two sessions' journals in the same store instance never collide).
agentengine_add_test(test_rt_effect_journal LIBS agentengine::core agentengine_warnings)

if(AGENTENGINE_WITH_HTTPS)
  # ---- Milestone 5 Phase J2 (roadmap exit criterion, 018 §7 G2): plant a canary secret, scan every
  # persisted artifact this milestone's mechanisms actually produce (recordings, checkpoints,
  # error-path text) for zero occurrences, paired with a positive control proving the secret really
  # was exercised as a live credential.
  # ADR-037: ported off quark::TestKit<Session>/core::AgentSession onto agentengine::rt::AgentSession
  # for the checkpoint leg (5) -- supersedes the old test_secret_hygiene_canary_scan.cpp (removed).
  agentengine_add_test(test_rt_secret_hygiene_canary_scan
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()

# ---- ADR-037 Phase 2 -- porting G5 (test_model_call_gateway.cpp's own "AgentSession integration"
# block, the ONE case in that file that touched AgentSession/quark::TestKit at all) onto
# agentengine::rt::AgentSession directly. G1-G4/G6-G9 exercise ModelCallGateway/
# MiddlewareModelCallGateway standalone with no AgentSession involvement and are left untouched in
# test_model_call_gateway.cpp above.
agentengine_add_test(test_rt_model_call_gateway_session LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 1 -- agentengine::rt::task<T>, the first piece of AgentEngine's own runtime
# substrate (include/agentengine/rt/task.hpp), standalone and NOT yet wired into core/task.hpp.
agentengine_add_test(test_rt_task LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 1 -- agentengine::rt::channel<T> (include/agentengine/rt/channel.hpp), a bounded,
# thread-safe, backpressured producer/consumer channel meant to eventually replace
# quark::ReplyStream<T*> as core/stream.hpp's internal backend. Standalone, zero quark:: dependency,
# NOT yet wired into core/stream.hpp.
agentengine_add_test(test_rt_channel LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 1 -- agentengine::rt::SessionStore, the host-injected persistence interface
# (include/agentengine/rt/session_store.hpp) replacing quark::EventLog/FenceToken, standalone and NOT
# yet wired into core/agent_session.hpp's snapshot/checkpoint functions.
agentengine_add_test(test_rt_session_store LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 1 -- agentengine::rt::ThreadPool, the minimal executor for driving task<void> job
# completion off the calling thread (include/agentengine/rt/thread_pool.hpp), standalone and NOT yet
# wired into any live Quark-based code.
agentengine_add_test(test_rt_thread_pool LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-175 (issue #78) -- a parked coroutine is resumed by its home (the block_on() driving
# it, including every ThreadPool job), and AsyncMutex ownership is a holder id. Issue #78's reproduction
# plus every red-team finding against the intermediate designs. Memory-capped, self-watchdogged.
agentengine_add_test(test_rt_parked_task_home LIBS agentengine::core agentengine_warnings TIMEOUT 300)

# ---- ADR-037 Phase 1 -- agentengine::rt::CircuitBreaker, a self-contained reimplementation of
# quark::CircuitBreaker's algorithm (include/agentengine/rt/circuit_breaker.hpp), standalone and NOT
# yet wired into core/model_call_gateway.hpp.
agentengine_add_test(test_rt_circuit_breaker LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2 -- agentengine::rt::AsyncMutex, I1's local replacement now that AgentSession
# stops being a quark::Actor.
agentengine_add_test(test_rt_async_mutex LIBS agentengine::core agentengine_warnings)

# ---- ADR-064 Design B -- agentengine::rt::drive_leaf_task<T>(), the narrow "leaf task" driver
# VectorRagContextProvider::recall's invoke uses (include/agentengine/rt/drive_leaf_task.hpp).
agentengine_add_test(test_rt_drive_leaf_task LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 -- agentengine::rt::AppendLogStore, the append-only persistence primitive both
# rt::WorkflowSupervisor's own time-travel gap and rt::ProjectRegistry's own archived-tail gap name
# as their shared missing dependency. Not wired into either yet -- proven standalone first.
agentengine_add_test(test_rt_append_log_store LIBS agentengine::core agentengine_warnings)
# ADR-195 E32 red team round 3: the store's headers must not leak <windows.h> into an includer. The
# check is the compile itself (see the file's own top comment).
agentengine_add_test(test_rt_append_log_store_header_hygiene LIBS agentengine::core agentengine_warnings)

# ---- ADR-181 phase 1 -- rt::BackgroundJobRunner, the in-memory core: per-job cancel, run cascade,
# Background<N> counting cancel_requested jobs, allowlist EffectContext, progress sink, shutdown.
agentengine_add_test(test_rt_background_job_runner LIBS agentengine::core agentengine_warnings TIMEOUT 120)

# v1 slice of docs/planning/dynamic-multi-agent-fanout-design-draft.md (rounds 1-14) --
# agentengine::rt::multi_agent::{Budget, spawn, spawn_with_retry, parallel}
# (include/agentengine/rt/multi_agent.hpp). Real positive/negative controls, including a genuine
# multi-threaded Budget contention test and a ThreadPool-backed parallel() fan-out, per this project's
# own "a real concurrency test under rt::ThreadPool, not a synchronous driver" bar.
agentengine_add_test(test_rt_multi_agent LIBS agentengine::core agentengine_warnings)

# ---- ADR-037 Phase 2 -- 013 §6 G3's cross-surface equivalence proof, ported onto rt::AgentSession
# directly for its AgentSession-driven setup -- supersedes test_cross_surface_equivalence.cpp.
agentengine_add_test(test_rt_cross_surface_equivalence LIBS agentengine::core agentengine_warnings)

agentengine_add_test(test_block_on_exception LIBS agentengine::core agentengine_warnings TIMEOUT 60)

# ---- ADR-102 Phase 4 -- agentengine::rt::block_on<T>() (rt/block_on.hpp), the real fix for a real,
# independent-red-team-found, empirically-proven AsyncMutex mutual-exclusion violation the naive
# "while (!t.done()) t.resume();" drive loop has under genuine cross-thread contention. Real OS threads
# (<thread>), no Docker/Ledger needed -- agentengine::core alone provides AsyncMutex/task<T>.
agentengine_add_test(test_rt_block_on LIBS agentengine::core agentengine_warnings)

# ---- decisions/ADR-168-bounded-reflection-loop.md -- issue #55: run_with_bounded_reflection()
# (rt/bounded_reflection.hpp), a thin outer driver over repeated AgentSession::start_run() calls.
agentengine_add_test(test_bounded_reflection LIBS agentengine::core agentengine_warnings)

# ---- GitHub issue #48 -- pal/console.hpp, the one place a Windows console is put into UTF-8 so a
# real model's em dashes and accented letters do not reach the terminal as OEM-code-page mojibake.
# Header-only and daemon-free; reads the code page back through Win32 rather than inspecting glyphs,
# since a test's own stdout under ctest is a pipe where nothing is transcoded at all.
agentengine_add_test(test_console_utf8 LIBS agentengine::core agentengine_warnings)
# Every console wait inside the test is already bounded, but the input half blocks on a real console
# handle, so a hard ceiling here too: a hung console must fail this leg, never stall it.
set_tests_properties(test_console_utf8 PROPERTIES TIMEOUT 60)
