# tests/plugin/: the WASM plugin backend (src/backends/wasm/).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "plugin" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- wasmtime C API smoke proof (M2 Phase D task D1, decision 5) -------------------------------
# Only built when AGENTENGINE_WITH_WASM is explicitly turned ON (root CMakeLists.txt) -- proves the
# vendored dependency itself links and runs a real WebAssembly module, independent of the real
# SandboxBackend built further down (D3, decisions/ADR-010-*.md) and its tool-pipeline bridge
# (decisions/ADR-040-*.md) -- this smoke test predates both and is kept as the narrowest possible
# "does the dependency itself work" check.
if(AGENTENGINE_WITH_WASM)
  agentengine_add_test(test_wasmtime_smoke LIBS agentengine::wasmtime_vendor)
  if(WIN32)
    # Windows has no rpath; wasmtime.dll must be reachable via PATH at run time.
    set_tests_properties(test_wasmtime_smoke PROPERTIES
      ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}")
  endif()
  # Linux links libwasmtime.a (static) -- see root CMakeLists.txt's AGENTENGINE_WITH_WASM block for
  # why -- so no run-time library path wiring is needed there.

  # ---- wasm SandboxBackend proof (M2 Phase D task D3, decisions/ADR-010-*.md) -------------------
  # Builds tests/fixtures/wasm_ae_tool_fixture/'s real ae:tool component via cargo-component when
  # that toolchain is present (checked with find_program, never assumed) -- the compiled .wasm is
  # NOT committed to the repo (.gitignore's own "WASM plugin build output" rule, project-wide, not
  # a rule this task carves an exception into). When the toolchain is absent, the test itself
  # SKIPs (SKIP_RETURN_CODE 77, the same mechanism test_shell_runner_no_process_creation already
  # uses for "tool not found on this machine") rather than failing or silently reporting a false
  # pass.
  find_program(AE_CARGO_EXE cargo)
  find_program(AE_CARGO_COMPONENT_EXE cargo-component)
  set(AE_WASM_FIXTURE_DIR "${AE_TESTS_SOURCE_DIR}/fixtures/wasm_ae_tool_fixture")
  set(AE_WASM_FIXTURE_WASM
    "${AE_WASM_FIXTURE_DIR}/target/wasm32-unknown-unknown/release/ae_tool_fixture.wasm")
  if(AE_CARGO_EXE AND AE_CARGO_COMPONENT_EXE)
    add_custom_command(
      OUTPUT "${AE_WASM_FIXTURE_WASM}"
      COMMAND "${AE_CARGO_EXE}" component build --release --target wasm32-unknown-unknown
      WORKING_DIRECTORY "${AE_WASM_FIXTURE_DIR}"
      DEPENDS
        "${AE_WASM_FIXTURE_DIR}/src/lib.rs"
        "${AE_WASM_FIXTURE_DIR}/Cargo.toml"
        "${AE_WASM_FIXTURE_DIR}/wit/ae-tool.wit"
      COMMENT "Building D3 test fixture (cargo-component): ae_tool_fixture.wasm")
    add_custom_target(ae_wasm_fixture ALL DEPENDS "${AE_WASM_FIXTURE_WASM}")
  endif()

  add_executable(test_wasm_backend test_wasm_backend.cpp)
  target_link_libraries(test_wasm_backend PRIVATE agentengine::wasm_backend)
  target_compile_definitions(test_wasm_backend PRIVATE
    AE_WASM_FIXTURE_PATH="${AE_WASM_FIXTURE_WASM}")
  if(TARGET ae_wasm_fixture)
    add_dependencies(test_wasm_backend ae_wasm_fixture)
  endif()
  add_test(NAME test_wasm_backend COMMAND test_wasm_backend)
  set_tests_properties(test_wasm_backend PROPERTIES SKIP_RETURN_CODE 77)
  if(WIN32)
    set_tests_properties(test_wasm_backend PROPERTIES
      ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}")
  endif()

  # ---- wasm tool-pipeline bridge proof (decisions/ADR-040-wasm-tool-pipeline-bridge.md) ----------
  # Wires WasmBackend-discovered tools into the real core/tool_pipeline.hpp ToolTable/invoke_tool --
  # same fixture/toolchain-availability posture as test_wasm_backend above (SKIP, not fail, when
  # cargo-component is unavailable); its own pure-logic wasm_tool_result_to_json() checks still run
  # regardless, see this test file's own header comment.
  add_executable(test_wasm_tool_bridge test_wasm_tool_bridge.cpp)
  target_link_libraries(test_wasm_tool_bridge PRIVATE agentengine::wasm_backend)
  target_compile_definitions(test_wasm_tool_bridge PRIVATE
    AE_WASM_FIXTURE_PATH="${AE_WASM_FIXTURE_WASM}")
  if(TARGET ae_wasm_fixture)
    add_dependencies(test_wasm_tool_bridge ae_wasm_fixture)
  endif()
  add_test(NAME test_wasm_tool_bridge COMMAND test_wasm_tool_bridge)
  set_tests_properties(test_wasm_tool_bridge PROPERTIES SKIP_RETURN_CODE 77)
  if(WIN32)
    set_tests_properties(test_wasm_tool_bridge PROPERTIES
      ENVIRONMENT "PATH=${AGENTENGINE_WASMTIME_DLL_DIR};$ENV{PATH}")
  endif()
endif()
