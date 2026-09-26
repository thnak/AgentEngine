# tests/tools/: built-in agent tools (include/agentengine/tools/).
# Added by tests/CMakeLists.txt, which also sets the flags, output directory, working directory and
# the "tools" ctest label for every test here (cmake/AgentEngineTests.cmake).

# ---- ReadContent, the first shipped first-party tool (009 §7's "Content reading" catalog entry,
# first-party-tools Track A) -- URL-source only, gated by NetOut via sandbox::HostEgressProxy, so it
# needs the real egress-proxy static library even though the tests themselves exercise a fake
# NetEgressBackend conformer (tools/read_content.hpp's own file-top comment explains the scope cut).
agentengine_add_test(test_read_content
  LIBS agentengine::core agentengine::net_egress_proxy agentengine_warnings)

# ---- ExtractPdfText, 009 §7's PDF-text-layer document-extraction candidate (ADR-106, docs/planning/
# pdf-text-extraction-design-draft.md Revision 7) -- only built when AGENTENGINE_WITH_PDF is
# explicitly turned ON (root CMakeLists.txt), the same opt-in posture AGENTENGINE_WITH_WASM/
# AGENTENGINE_BUILD_PYTHON_RUNNER already have for a big, untrusted-input-handling dependency. Real
# end-to-end: spawns the REAL sandboxed agentengine_pdf_worker against a real, embedded minimal PDF.
if(AGENTENGINE_WITH_PDF)
  agentengine_add_test(test_extract_pdf_text
    LIBS agentengine::core agentengine::net_egress_proxy agentengine::extract_pdf_text
      agentengine_warnings)
  # Spawns a real sandboxed child process (AppContainer/Job Object on Windows, namespaces/cgroups on
  # Linux) and runs a real PDFium decode -- generous but finite headroom over ordinary run time.
  set_tests_properties(test_extract_pdf_text PROPERTIES TIMEOUT 60)

  # ---- ExtractPdfToc, a second candidate sharing the same sandboxed worker (extract_pdf_toc.hpp's
  # own file-top comment explains why this one shares rather than duplicates).
  agentengine_add_test(test_extract_pdf_toc
    LIBS agentengine::core agentengine::net_egress_proxy agentengine::extract_pdf_text
      agentengine_warnings
    TIMEOUT 60)

  # ---- ExtractPdfImages, a third candidate sharing the same sandboxed worker -- embedded-image
  # METADATA listing only (position/dimensions/format, never bytes; extract_pdf_images.hpp's own
  # file-top comment explains the scope cut and why this shares rather than duplicates).
  agentengine_add_test(test_extract_pdf_images
    LIBS agentengine::core agentengine::net_egress_proxy agentengine::extract_pdf_text
      agentengine_warnings
    TIMEOUT 60)

  # ---- ExtractPdfMetadata, a fourth candidate sharing the same sandboxed worker
  # (extract_pdf_metadata.hpp's own file-top comment explains why this one shares rather than
  # duplicates).
  agentengine_add_test(test_extract_pdf_metadata
    LIBS agentengine::core agentengine::net_egress_proxy agentengine::extract_pdf_text
      agentengine_warnings
    TIMEOUT 60)
endif()

# ---- EffectContext::sandbox_fs seam (first-party-tools follow-up, Phase 0): tools/
# read_sandbox_file.hpp's worked example, proven against a real MediatedFileSystemAdapter.
agentengine_add_test(test_effect_context_sandbox_fs
  LIBS agentengine::core agentengine::mediated_shell_runner agentengine::worktree_store
    agentengine_warnings)
