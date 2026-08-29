// Proves tools/extract_pdf_text.hpp's ExtractPdfText tool -- 009 §7's PDF-text-layer document-
// extraction candidate, docs/planning/pdf-text-extraction-design-draft.md Revision 7. Two tiers:
//
//   1. Pure-logic unit tests against extract_pdf_text_detail::parse_worker_output()/build_reply() --
//      no sandbox, no real PDFium, hand-crafted binary record streams (mirrors what a real/killed/
//      self-limited pdf_worker_main.cpp process would actually produce).
//   2. A REAL end-to-end test: a genuine, hand-verified-valid minimal PDF (embedded below, byte-for-
//      byte the same file this branch's own manual `agentengine_pdf_worker.exe test.pdf` run was
//      checked against directly before this test was written -- confirmed to report
//      total_page_count=1 and extract "Hello PDFium Test") goes through the REAL sandboxed worker
//      (NativeJailBackend::create()/exec() on Windows, LinuxNativeJailBackend on Linux) via
//      ExtractPdfText::invoke_via, using a FAKE FileSystemAdapter for the SOURCE fetch only (the
//      same "fake, not real I/O" pattern test_read_content.cpp's own FakeFileSystemAdapter already
//      establishes) -- the tool's own scratch-file write and the worker invocation are both 100%
//      real; only where the SOURCE bytes originally came from is faked, since that half is already
//      proven by test_read_content.cpp/test_effect_context_sandbox_fs.cpp's own suites.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "agentengine/tools/extract_pdf_text.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using agentengine::result;
namespace pdf_detail = agentengine::tools::extract_pdf_text_detail;

// ---- Tier 1: pure-logic tests -------------------------------------------------------------------

std::string make_record(std::uint32_t kind, std::uint32_t index, std::string const& payload) {
    std::string out;
    out.resize(16);
    std::uint32_t header[4] = {pdf_detail::kMagic, kind, index,
                                static_cast<std::uint32_t>(payload.size())};
    std::memcpy(out.data(), header, 16);
    out += payload;
    return out;
}

void test_parse_worker_output_happy_path() {
    std::string raw = make_record(pdf_detail::kKindMeta, 2, "");
    raw += make_record(pdf_detail::kKindPage, 0, "page zero text");
    raw += make_record(pdf_detail::kKindPage, 1, "page one text");

    auto parsed = pdf_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a well-formed record stream parses");
    if (parsed) {
        check(parsed->total_page_count == 2, "META's index carries the real total page count");
        check(parsed->pages.size() == 2, "both PAGE records are captured");
        if (parsed->pages.size() == 2) {
            check(parsed->pages[0] == "page zero text", "page 0's text is exact");
            check(parsed->pages[1] == "page one text", "page 1's text is exact");
        }
    }
}

void test_parse_worker_output_truncated_trailing_record_is_discarded() {
    std::string raw = make_record(pdf_detail::kKindMeta, 5, "");
    raw += make_record(pdf_detail::kKindPage, 0, "complete page");
    // A record claiming a payload_len far longer than what's actually present -- exactly what a
    // worker killed mid-write (wall_ms/memory cap) would leave behind.
    std::string partial_header(16, '\0');
    std::uint32_t header[4] = {pdf_detail::kMagic, pdf_detail::kKindPage, 1, 9999};
    std::memcpy(partial_header.data(), header, 16);
    raw += partial_header;
    raw += "short";  // far less than the claimed 9999 bytes

    auto parsed = pdf_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a truncated trailing record does not fail the whole parse");
    if (parsed) {
        check(parsed->total_page_count == 5, "META is still read correctly");
        check(parsed->pages.size() == 1, "only the COMPLETE page record is kept -- the truncated "
                                          "trailing one is discarded, not miscounted");
        if (parsed->pages.size() == 1) {
            check(parsed->pages[0] == "complete page", "the one kept page's content is exact");
        }
    }
}

void test_parse_worker_output_non_contiguous_index_stops() {
    std::string raw = make_record(pdf_detail::kKindMeta, 3, "");
    raw += make_record(pdf_detail::kKindPage, 1, "skipped page 0");  // index 1, but pages is empty
    auto parsed = pdf_detail::parse_worker_output(raw);
    check(parsed.has_value(), "still parses (META alone is enough)");
    if (parsed) {
        check(parsed->pages.empty(),
              "a non-contiguous PAGE index (1, when 0 was expected) stops parsing rather than "
              "accepting an out-of-order record");
    }
}

void test_parse_worker_output_no_meta_is_an_error() {
    std::string raw = make_record(pdf_detail::kKindPage, 0, "orphan page, no META first");
    auto parsed = pdf_detail::parse_worker_output(raw);
    check(!parsed.has_value(), "a stream with no META record is rejected");
    if (!parsed) {
        check(parsed.error().code == "extract_pdf_text.worker_output_malformed",
              "the failure names exactly why");
    }
}

}  // namespace

// ---- Tier 2: real end-to-end test ---------------------------------------------------------------

namespace {

// Byte-for-byte the same minimal, hand-verified-valid single-page PDF this branch's own manual
// `agentengine_pdf_worker.exe` run was checked against directly (real PDFium, real output: META
// total_pages=1, one PAGE record containing "Hello PDFium Test") before this test was written.
constexpr char const* kTestPdf =
    "%PDF-1.4\n"
    "1 0 obj\n"
    "<< /Type /Catalog /Pages 2 0 R >>\n"
    "endobj\n"
    "2 0 obj\n"
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
    "endobj\n"
    "3 0 obj\n"
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << /Font << /F1 4 0 R >> >> "
    "/Contents 5 0 R >>\n"
    "endobj\n"
    "4 0 obj\n"
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n"
    "endobj\n"
    "5 0 obj\n"
    "<< /Length 48 >>\n"
    "stream\n"
    "BT /F1 24 Tf 20 100 Td (Hello PDFium Test) Tj ET\n"
    "endstream\n"
    "endobj\n"
    "xref\n"
    "0 6\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000058 00000 n \n"
    "0000000115 00000 n \n"
    "0000000241 00000 n \n"
    "0000000311 00000 n \n"
    "trailer\n"
    "<< /Size 6 /Root 1 0 R >>\n"
    "startxref\n"
    "409\n"
    "%%EOF";

// Same pattern as test_read_content.cpp's own FakeFileSystemAdapter -- returns canned bytes, no
// real disk I/O. ExtractPdfText's own scratch-file write (real) and worker invocation (real) are
// what this test actually proves; where the source bytes originate is already covered elsewhere.
class FakePdfFileSystemAdapter final : public agentengine::FileSystemAdapter {
public:
    result<std::vector<std::byte>> read_file(std::string_view) override {
        std::string const text(kTestPdf);
        return std::vector<std::byte>(reinterpret_cast<std::byte const*>(text.data()),
                                       reinterpret_cast<std::byte const*>(text.data()) + text.size());
    }
    result<void> write_file(std::string_view, std::span<std::byte const>, bool) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "not implemented", "test.not_implemented"});
    }
    result<void> remove(std::string_view, bool) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "not implemented", "test.not_implemented"});
    }
    result<void> rename(std::string_view, std::string_view) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "not implemented", "test.not_implemented"});
    }
    result<void> copy_file(std::string_view, std::string_view) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "not implemented", "test.not_implemented"});
    }
    result<void> make_directory(std::string_view, bool) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "not implemented", "test.not_implemented"});
    }
    result<std::vector<agentengine::DirEntry>> list_directory(std::string_view) override {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "not implemented", "test.not_implemented"});
    }
    result<bool> exists(std::string_view) override { return true; }
    result<std::string> canonicalize(std::string_view path) override { return std::string(path); }
};

void test_real_end_to_end_extraction() {
    using agentengine::tools::ExtractPdfText;

    agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
        {agentengine::cap::FsRead{"work", "", std::nullopt}});
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    FakePdfFileSystemAdapter adapter;
    ctx.sandbox_fs = &adapter;

    ExtractPdfText::Args args;
    args.path = "test.pdf";  // the fake adapter ignores this and returns kTestPdf regardless

    auto reply = ExtractPdfText::invoke(args, ctx);
    if (!reply.has_value()) {
        std::fprintf(stderr, "extract_pdf_text real end-to-end call failed: code=%s message=%s\n",
                     reply.error().code.c_str(), reply.error().message.c_str());
    }
    check(reply.has_value(), "a real PDF genuinely extracts through the real sandboxed worker end to end");
    if (reply) {
        check(reply->total_page_count == 1, "total_page_count matches the real 1-page document");
        check(reply->pages_processed == 1, "the single page was fully processed");
        check(!reply->truncated_pages, "not truncated -- the whole (tiny) document was processed");
        check(reply->preview.find("Hello PDFium Test") != std::string::npos,
              "the real extracted text is exactly what PDFium reported when run manually");
        check(!reply->truncated, "well under the preview threshold, not truncated");
    }
}

void test_ambiguous_source_rejected_before_any_sandbox_work() {
    using agentengine::tools::ExtractPdfText;
    agentengine::CapabilitySet const held;
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    ExtractPdfText::Args args;  // neither url nor path set
    auto reply = ExtractPdfText::invoke(args, ctx);
    check(!reply.has_value(), "neither url nor path set -> rejected");
    if (!reply) {
        check(reply.error().code == "extract_pdf_text.ambiguous_source",
              "ambiguity uses its own error code, the same shape ReadContent's own check has");
    }
}

void test_companion_skill_parses_for_real() {
    auto descriptor = agentengine::tools::make_extracting_document_text_skill_source();
    check(descriptor.origin_id == "extract-pdf-text", "the skill source's own origin_id is set");
    auto loaded = descriptor.load_skills();
    if (!loaded.has_value()) {
        std::fprintf(stderr, "skill parse error: code=%s message=%s\n", loaded.error().code.c_str(),
                     loaded.error().message.c_str());
    }
    check(loaded.has_value(), "the companion skill's SKILL.md actually parses (real parse, not just "
                               "written prose)");
    if (loaded) {
        check(loaded->size() == 1, "exactly one skill in this source");
        if (loaded->size() == 1) {
            check(loaded->front().skill.frontmatter.name == "extracting-document-text",
                  "the parsed skill's own name matches");
            auto const& allowed = loaded->front().skill.frontmatter.allowed_tools;
            check(std::find(allowed.begin(), allowed.end(), "extract_pdf_text") != allowed.end(),
                  "allowed-tools names extract_pdf_text");
            check(std::find(allowed.begin(), allowed.end(), "extract_pdf_toc") != allowed.end(),
                  "allowed-tools also names extract_pdf_toc -- one skill, two tools, not a second "
                  "skill duplicating this one");
        }
    }
}

}  // namespace

int main() {
    test_parse_worker_output_happy_path();
    test_parse_worker_output_truncated_trailing_record_is_discarded();
    test_parse_worker_output_non_contiguous_index_stops();
    test_parse_worker_output_no_meta_is_an_error();
    test_ambiguous_source_rejected_before_any_sandbox_work();
    test_companion_skill_parses_for_real();
    test_real_end_to_end_extraction();

    if (g_failures == 0) {
        std::printf("test_extract_pdf_text: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_extract_pdf_text: %d check(s) failed\n", g_failures);
    return 1;
}
