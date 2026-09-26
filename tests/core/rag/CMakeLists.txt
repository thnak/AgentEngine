# tests/core/rag/: retrieval: vector/sparse indexes, corpus ingestion, RAG context providers (ADR-063, ADR-180).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "core/rag" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- ADR-063 (retrieval-augmented ContextProvider shape) -- foundation layer, Proposed status ----
# §2.6b / §3 claim 6: neutralize_forged_provenance_markers() (core/provenance_marker.hpp), the
# marker-forgery defense generalized out of memory_provider.hpp's own gap-17 fix.
if(WIN32)
  agentengine_add_test(test_provenance_marker
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# §2.3 / §4 finding 7: BruteForceCosineIndex (core/vector_index.hpp) -- ranking correctness, contract
# rejection, and the deterministic score-desc/id-asc tie-break.
if(WIN32)
  agentengine_add_test(test_vector_index
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  agentengine_add_test(test_vector_index_benchmark LIBS agentengine::core agentengine_warnings)

  # decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.2/§3 claim 3 --
  # `BM25Index` (core/sparse_index.hpp): hand-computed formula correctness, contract rejection,
  # deterministic tie-break.
  agentengine_add_test(test_sparse_index LIBS agentengine::core agentengine_warnings)
endif()

# §2.6a / §3 claim 5: rag_corpus_key()/rag_corpus_mount() (core/corpus_scope.hpp) -- multi-tenant
# corpus isolation, mirroring test_memory_cross_tenant_isolation.cpp's proven shape.
if(WIN32)
  agentengine_add_test(test_corpus_scope
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# §2.4 / §4 findings 1-2: CorpusChunkRecord (core/corpus_chunk.hpp) -- the shared citation-metadata
# contract CorpusSource (writer) and VectorRagContextProvider (reader) both build against.
if(WIN32)
  agentengine_add_test(test_corpus_chunk
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# §2.4/§2.4b, closes §4 findings 2/8/9 for its own call site: DiskCorpusSource + RecursiveChunker
# (core/corpus_source.hpp) -- folder-mount ingestion, re-mount dedup, chunking.
if(WIN32)
  agentengine_add_test(test_corpus_source
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

# §2.1/§2.1b/§2.6a/§2.6b, closes the "full claim" gap for §3 claims 5/6: VectorRagContextProvider
# (core/vector_rag_context_provider.hpp) -- the real ContextProvider conformer.
if(WIN32)
  agentengine_add_test(test_vector_rag_context_provider
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.3/§3 claims 2, 8 --
  # `HybridRagContextProvider` (core/hybrid_rag_context_provider.hpp): RRF fusion end-to-end,
  # citation rendering/forgery-defense reuse, recall tool.
  agentengine_add_test(test_hybrid_rag_context_provider
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)

  # decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.1, §4 findings R1/R2 --
  # `RemoteVectorIndex`/`AnyVectorIndex` (core/remote_vector_index.hpp): concept disjointness, and
  # real (not merely mock-compiles) instantiation of every RemoteVectorIndex dispatch branch in
  # VectorRagContextProvider, HybridRagContextProvider, and DiskCorpusSource::mount_hybrid().
  agentengine_add_test(test_remote_vector_index
    LIBS agentengine::core agentengine::worktree_store agentengine_warnings)
endif()

if(AGENTENGINE_WITH_HTTPS)
  # ---- decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.5/§3 claim 6:
  # QdrantVectorIndex (protocol/qdrant/vector_index.hpp) -- offline-provable surface: point-id
  # derivation, request/response construction, error-envelope parsing. No server, runs
  # unconditionally, mirrors test_openai_embedder.cpp's own offline split. The live-network half
  # is test_qdrant_vector_index_live_e2e below (passed against real Qdrant 1.19.1, 2026-09-23).
  agentengine_add_test(test_qdrant_vector_index
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)

  # ---- ADR-180 §3 claim 6's live half: QdrantVectorIndex against a REAL Qdrant instance -----------
  agentengine_add_test(test_qdrant_vector_index_live_e2e
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  set_tests_properties(test_qdrant_vector_index_live_e2e PROPERTIES LABELS "live-network" TIMEOUT 300)
endif()

# decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.6/§3 claim 7 -- VulkanCosineIndex
# (src/backends/vulkan_vector_index/): basic correctness, VectorIndex conformance, and the
# epsilon-bounded GPU-vs-CPU score comparison at dim=1536, n in {1000, 5000}. Only built when
# AGENTENGINE_WITH_VULKAN is explicitly turned ON (root CMakeLists.txt) -- the test binary itself
# fails closed (SKIP, exit 0) at RUN time if no Vulkan-capable device is present on the machine
# actually running ctest, matching AGENTENGINE_WITH_HTTPS-gated live-network tests' own SKIP-not-FAIL
# posture for an environment precondition this repo cannot guarantee on every CI/dev machine.
if(AGENTENGINE_WITH_VULKAN)
  agentengine_add_test(test_vulkan_cosine_index
    LIBS agentengine::core agentengine::vulkan_vector_index agentengine_warnings)

  # ADR-180 §4e (red-team pass 5): executes every checked-VkResult failure branch through a TEST-ONLY
  # fault-injection seam. The seam exists only when vulkan_cosine_index.cpp is compiled with
  # AGENTENGINE_VULKAN_FAULT_INJECTION, so this is a SEPARATE library variant built from the same
  # source -- the production agentengine::vulkan_vector_index target above never defines it.
  add_library(agentengine_vulkan_vector_index_fault_injection STATIC
    "${PROJECT_SOURCE_DIR}/src/backends/vulkan_vector_index/vulkan_cosine_index.cpp")
  target_include_directories(agentengine_vulkan_vector_index_fault_injection
    PUBLIC "${PROJECT_SOURCE_DIR}/src" PRIVATE "${AE_SPIRV_GENERATED_DIR}")
  target_link_libraries(agentengine_vulkan_vector_index_fault_injection PUBLIC agentengine::core)
  target_link_libraries(agentengine_vulkan_vector_index_fault_injection PRIVATE agentengine_warnings Vulkan::Vulkan)
  target_compile_definitions(agentengine_vulkan_vector_index_fault_injection
    PUBLIC AGENTENGINE_WITH_VULKAN AGENTENGINE_VULKAN_FAULT_INJECTION)
  agentengine_add_test(test_vulkan_cosine_index_fault_injection
    LIBS agentengine::core agentengine_vulkan_vector_index_fault_injection agentengine_warnings)
endif()
