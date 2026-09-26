# tests/core/chat/: ChatClient, recording/replay, model-call gateways and streaming (004).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "core/chat" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- decisions/ADR-069-content-triggered-model-response-replay.md's prove phase: ContentReplayGateway
# -- a settled response's content-triggered discard-and-retry, wrapping any ModelCallGatewayLike,
# bounded both per-trigger-site and across a session's lifetime independently.
agentengine_add_test(test_content_replay_gateway LIBS agentengine::core agentengine_warnings)

if(AGENTENGINE_WITH_HTTPS)
  # ---- ADR-023 Phase 1: response-format codec (decisions/ADR-023-response-format-codec-seam.md) ----
  # Pure logic (core/response_format_codec.hpp) proven directly against real Harmony/DeepSeek/Hermes/
  # <think> example strings -- no network, no ChatClient backend involved, only core content types.
  agentengine_add_test(test_response_format_codec LIBS agentengine::core agentengine_warnings)

  # ---- GitHub issue #72 -- both streaming accumulators cast a provider-supplied tool-call /
  # content-block index straight to std::size_t after checking only that it was a number. A negative
  # one became SIZE_MAX, the resize that looked like a bounds check wrapped to resize(0), and the
  # next line wrote out of bounds: segfault in Release, on one SSE chunk. Offline, both providers,
  # with a positive control that the ordinary stream still parses.
  agentengine_add_test(test_chat_stream_index_hardening
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings)

  # ---- ADR-173 (GitHub issue #61): the system-channel taint fence, proven on BOTH serializers ----
  # Lives with the protocol tests (not core) because its claim is about the WIRE bytes two different
  # backends produce, which is what needs the provider_http_client link both chat clients pull in.
  agentengine_add_test(test_system_channel_taint_fence
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings)

  # ---- ADR-019: genuinely incremental streaming (Phase C/D2's last self-contained residual) -------
  # Asserts time-to-first-item against a drip server, which is the one currency that distinguishes
  # incremental delivery from replay-after-buffering, plus a byte-by-byte feed of the framing
  # decoders (their whole risk is state across call boundaries). Deterministic, default suite.
  agentengine_add_test(test_chat_client_stream_incremental
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto
    TIMEOUT 120)

  # ADR-177 Q1: a stream killed mid-body on the REAL OpenAI client + REAL transport reaches AgentSession as
  # something its retry decision accepts. Loopback server, default suite.
  agentengine_add_test(test_stream_retry_real_transport
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto
    TIMEOUT 120)

  # ---- Milestone 5 Phase J1 (roadmap exit criterion, 004 §7 G1): the same unchanged call site, run
  # against real OpenAI/Anthropic backends over local TLS servers, a ReplayChatClient, and a
  # FailoverChatClient<real, real> composition with a genuinely failing primary.
  agentengine_add_test(test_chat_client_cross_backend_parity
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)

  # ---- Milestone 5 Phase J3 (roadmap exit criterion, 004 §7 G2): the common-case (fast provider)
  # bounded-release proof, plus a real, quantified finding that chat_stream()'s own fetch has no
  # stop_token wired to the consumer's cancellation -- named honestly, not silently claimed complete.
  agentengine_add_test(test_chat_client_stream_cancellation_bounded
    LIBS agentengine::core agentengine::provider_http_client agentengine_warnings MbedTLS::mbedtls
      MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()

# ---- RecordedChatClient fixture player (test-only) ---------------------------------------------
# Proves the ChatClient concept and ChatRequest/ChatResponse vocabulary are usable end-to-end
# against hand-authored JSON fixtures, without a real network call or API key (CLAUDE.md task
# brief; concept: 004-Model-Provider-Plane.md §6).
# nlohmann/json is a test-only dependency, FetchContent-ed in tests/CMakeLists.txt.
add_executable(test_recorded_chat_client test_recorded_chat_client.cpp)
target_link_libraries(test_recorded_chat_client PRIVATE
  agentengine::core agentengine_warnings nlohmann_json::nlohmann_json)
target_compile_definitions(test_recorded_chat_client PRIVATE
  AE_TEST_FIXTURE_DIR="${AE_TESTS_SOURCE_DIR}/fixtures/chat_client")

add_test(NAME test_recorded_chat_client COMMAND test_recorded_chat_client)

# ---- Milestone 5 Phase B1/B2/B5/B6 -- the ChatClient concept made real -------------------------
# ChatRequest.tools/output_schema_json, the completed ChatClientCapabilities bitset,
# select_output_schema_strategy (004 §2's degradation rule), ChatClientRegistry.
agentengine_add_test(test_chat_client_provider_plane LIBS agentengine::core agentengine_warnings)

# ---- Milestone 5 Phase B3 -- outbound-credential resolution at the point of use (004 §1/018 §4) --
agentengine_add_test(test_chat_client_credential_resolution LIBS agentengine::core agentengine_warnings)

# ---- Milestone 5 Phase B4b -- ae::stream<T> (core/stream.hpp), the AgentEngine-side adapter over
# Quark's ReplyStream/StreamChannel credit-controlled ring (RFC 024/ADR-018) -- against a real
# ChatClient conformer that streams for real across a background producer thread.
agentengine_add_test(test_chat_client_stream LIBS agentengine::core agentengine_warnings)

# ---- Middleware<Ms...>, model-call interception point (002 §5, ADR-033), proven through
# MiddlewareModelCallGateway (ADR-036) -- was test_middleware_chat_client.cpp/MiddlewareChatClient
# until both were removed 2026-08-12.
agentengine_add_test(test_middleware_model_call_gateway LIBS agentengine::core agentengine_warnings)

# ---- ADR-036 -- ModelCallGateway/MiddlewareModelCallGateway (retry+breaker+failover+middleware as
# a real coroutine, AgentSession-pluggable without a template-signature change).
add_executable(test_model_call_gateway test_model_call_gateway.cpp)
target_include_directories(test_model_call_gateway PRIVATE "${AE_TESTS_SOURCE_DIR}")
target_link_libraries(test_model_call_gateway PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_model_call_gateway COMMAND test_model_call_gateway)

# ---- ADR-148 -- RoutingModelCallGateway<SelectFn, Routes...> (GitHub issue #16 Finding 1), a THIRD
# ModelCallGatewayLike conformer alongside ModelCallGateway/MiddlewareModelCallGateway above.
add_executable(test_routing_model_call_gateway test_routing_model_call_gateway.cpp)
target_include_directories(test_routing_model_call_gateway PRIVATE "${AE_TESTS_SOURCE_DIR}")
target_link_libraries(test_routing_model_call_gateway PRIVATE agentengine::core agentengine_warnings)
add_test(NAME test_routing_model_call_gateway COMMAND test_routing_model_call_gateway)

# ---- ADR-048 (gap-audit finding 19, Phase 1) -- fail-closed Media capability gate, pure function.
agentengine_add_test(test_outbound_media_capability_gate LIBS agentengine::core agentengine_warnings)

# ---- Milestone 5 Phase G1 -- ChatCallRecording JSON codec (core/chat_recording.hpp), the shared
# foundation RecordingChatClient<Inner> and ReplayChatClient both depend on.
agentengine_add_test(test_chat_recording_codec LIBS agentengine::core agentengine_warnings)

# ---- Milestone 5 Phase G1 -- RecordingChatClient<Inner> (core/recording_chat_client.hpp), the real
# recording mechanism promoted from RecordedChatClient's test-scoped stand-in.
agentengine_add_test(test_recording_chat_client LIBS agentengine::core agentengine_warnings)

# ---- Milestone 5 Phase G2 -- ReplayChatClient (core/replay_chat_client.hpp), the real replay
# mechanism with identical chunk boundaries and inter-chunk timing (004 §7 G3).
agentengine_add_test(test_replay_chat_client LIBS agentengine::core agentengine_warnings)

add_executable(test_stream_delta_coalescing test_stream_delta_coalescing.cpp)
target_link_libraries(test_stream_delta_coalescing PRIVATE agentengine::core agentengine_warnings)
if(AGENTENGINE_WITH_HTTPS)
  # P1-P4 drive the real Anthropic/OpenAI stream accumulators, which only exist in HTTPS builds.
  target_link_libraries(test_stream_delta_coalescing PRIVATE agentengine::provider_http_client)
endif()
add_test(NAME test_stream_delta_coalescing COMMAND test_stream_delta_coalescing)
set_tests_properties(test_stream_delta_coalescing PROPERTIES TIMEOUT 60)
