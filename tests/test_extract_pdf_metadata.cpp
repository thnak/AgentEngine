// Proves tools/extract_pdf_metadata.hpp's ExtractPdfMetadata tool -- 009 §7's PDF document-
// information-dictionary metadata candidate, sharing extract_pdf_text.hpp's own sandboxed worker/
// sourcing infrastructure (its own file-top comment explains why this one shares rather than
// duplicates). Same two-tier shape as test_extract_pdf_text.cpp/test_extract_pdf_toc.cpp: pure-logic
// parse tests, then a REAL end-to-end test against a genuine, hand-verified-valid PDF with a real
// /Info dictionary (byte-for-byte the same file this branch's own manual
// `agentengine_pdf_worker.exe meta <path>` run was checked against directly before this test was
// written -- confirmed to report Title "Test Document Title", Author "Jane Q. Author", Subject
// "A Test Subject", Keywords "pdf, metadata, test", Creator "AgentEngine Test Suite", Producer
// "AgentEngine PDF Test Generator", CreationDate "D:20260829120000Z", ModDate "D:20260829130000Z",
// and total_page_count 2).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "agentengine/tools/extract_pdf_metadata.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using agentengine::result;
namespace meta_detail = agentengine::tools::extract_pdf_metadata_detail;

// ---- Tier 1: pure-logic tests -------------------------------------------------------------------

std::string make_meta_record(std::uint32_t total_pages) {
    std::string out;
    out.resize(16);
    std::uint32_t header[4] = {meta_detail::kMagic, meta_detail::kKindMeta, total_pages, 0};
    std::memcpy(out.data(), header, 16);
    return out;
}

std::string make_field_record(std::uint32_t tag_index, std::string const& value) {
    std::string out;
    out.resize(16);
    std::uint32_t header[4] = {meta_detail::kMagic, meta_detail::kKindMetaField, tag_index,
                                static_cast<std::uint32_t>(value.size())};
    std::memcpy(out.data(), header, 16);
    out += value;
    return out;
}

void test_parse_worker_output_happy_path_multiple_fields() {
    std::string raw = make_meta_record(5);
    raw += make_field_record(meta_detail::kTagTitle, "My Title");
    raw += make_field_record(meta_detail::kTagAuthor, "My Author");
    raw += make_field_record(meta_detail::kTagCreator, "My Creator");

    auto parsed = meta_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a well-formed META/META_FIELD record stream parses");
    if (parsed) {
        check(parsed->total_page_count == 5, "total_page_count comes from the META record's index");
        check(parsed->title.has_value() && *parsed->title == "My Title", "title is set and exact");
        check(parsed->author.has_value() && *parsed->author == "My Author", "author is set and exact");
        check(parsed->creator.has_value() && *parsed->creator == "My Creator",
              "creator is set and exact");
        check(!parsed->subject.has_value(), "subject was never emitted -- stays unset, not empty");
        check(!parsed->keywords.has_value(), "keywords was never emitted -- stays unset");
        check(!parsed->producer.has_value(), "producer was never emitted -- stays unset");
        check(!parsed->creation_date.has_value(), "creation_date was never emitted -- stays unset");
        check(!parsed->mod_date.has_value(), "mod_date was never emitted -- stays unset");
    }
}

void test_parse_worker_output_field_pdfium_reports_empty_is_absent() {
    // The worker never emits a META_FIELD record at all for a tag PDFium reports empty/absent
    // (sparse output, pdf_worker_main.cpp's own run_meta_mode) -- this is the SAME "no record" case
    // the previous test's subject/keywords/etc. already covers, made explicit as its own test: a
    // stream with only the META record and zero META_FIELD records leaves every optional unset.
    std::string raw = make_meta_record(3);
    auto parsed = meta_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a stream with only the META record parses");
    if (parsed) {
        check(parsed->total_page_count == 3, "total_page_count still comes through");
        check(!parsed->title.has_value() && !parsed->author.has_value() &&
                  !parsed->subject.has_value() && !parsed->keywords.has_value() &&
                  !parsed->creator.has_value() && !parsed->producer.has_value() &&
                  !parsed->creation_date.has_value() && !parsed->mod_date.has_value(),
              "a field PDFium reported empty for produces no record, so every field stays unset");
    }
}

void test_parse_worker_output_truncated_trailing_record_stops() {
    std::string raw = make_meta_record(2);
    raw += make_field_record(meta_detail::kTagTitle, "complete title");
    // A trailing record whose declared payload_len overruns the buffer.
    std::string partial_header(16, '\0');
    std::uint32_t header[4] = {meta_detail::kMagic, meta_detail::kKindMetaField,
                                meta_detail::kTagAuthor, 9999};
    std::memcpy(partial_header.data(), header, 16);
    raw += partial_header;
    raw += "short";

    auto parsed = meta_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a truncated trailing record does not fail the whole parse");
    if (parsed) {
        check(parsed->title.has_value() && *parsed->title == "complete title",
              "the complete record before the truncated one is still kept");
        check(!parsed->author.has_value(),
              "the truncated trailing record itself never gets applied");
    }
}

void test_parse_worker_output_unknown_tag_index_rejected() {
    std::string raw = make_meta_record(1);
    raw += make_field_record(99, "bogus tag");  // 99 is not any real tag index (0-7)

    auto parsed = meta_detail::parse_worker_output(raw);
    check(!parsed.has_value(), "an unknown META_FIELD tag index is rejected, not silently ignored");
    if (!parsed) {
        check(parsed.error().code == "extract_pdf_metadata.worker_output_malformed",
              "the unknown-tag-index rejection uses the worker-output-malformed error code");
    }
}

void test_parse_worker_output_no_meta_record_at_all_fails() {
    // No META record at all (worker crashed before emitting anything, or a corrupted stream) -- same
    // "no META record means the whole parse failed" contract extract_pdf_text_detail's own parser
    // already has, for the same reason (there is otherwise no way to know if the worker ran at all).
    std::string raw = make_field_record(meta_detail::kTagTitle, "orphaned field, no META record");
    auto parsed = meta_detail::parse_worker_output(raw);
    check(!parsed.has_value(), "a stream with only META_FIELD records and no META record fails");
    if (!parsed) {
        check(parsed.error().code == "extract_pdf_metadata.worker_output_malformed",
              "missing-META rejection uses the worker-output-malformed error code");
    }
}

void test_parse_worker_output_only_page_count_no_tag_fields_at_all() {
    // A PDF with no /Info dictionary at all (or PDFium reports every tag empty) -- zero META_FIELD
    // records, just the one META record. A valid, meaningful result, not an error.
    std::string raw = make_meta_record(7);
    auto parsed = meta_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a page-count-only stream (no /Info at all) is a valid result");
    if (parsed) {
        check(parsed->total_page_count == 7, "total_page_count is exact");
        check(!parsed->title.has_value() && !parsed->author.has_value() &&
                  !parsed->subject.has_value() && !parsed->keywords.has_value() &&
                  !parsed->creator.has_value() && !parsed->producer.has_value() &&
                  !parsed->creation_date.has_value() && !parsed->mod_date.has_value(),
              "every metadata field is unset when the document has no /Info dictionary at all");
    }
}

}  // namespace

// ---- Tier 2: real end-to-end test ---------------------------------------------------------------

namespace {

// Byte-for-byte the same minimal, hand-verified-valid 2-page PDF (with a real /Info dictionary
// referenced from the trailer's /Info entry) this branch's own manual
// `agentengine_pdf_worker.exe meta <path>` run was checked against directly before this test was
// written. Generated + byte-offset-verified via a small Python script (see this session's own
// scratchpad make_meta_pdf.py) rather than hand-computed, per this task's own required verification
// step. xref offsets below are the exact ones that script printed for this exact object layout.
constexpr char const* kTestPdfWithInfo =
    "%PDF-1.4\n"
    "1 0 obj\n"
    "<< /Type /Catalog /Pages 2 0 R >>\n"
    "endobj\n"
    "2 0 obj\n"
    "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\n"
    "endobj\n"
    "3 0 obj\n"
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << /Font << /F1 5 0 R >> >> "
    "/Contents 6 0 R >>\n"
    "endobj\n"
    "4 0 obj\n"
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << /Font << /F1 5 0 R >> >> "
    "/Contents 7 0 R >>\n"
    "endobj\n"
    "5 0 obj\n"
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n"
    "endobj\n"
    "6 0 obj\n"
    "<< /Length 44 >>\n"
    "stream\n"
    "BT /F1 24 Tf 20 100 Td (Page One Text) Tj ET\n"
    "endstream\n"
    "endobj\n"
    "7 0 obj\n"
    "<< /Length 44 >>\n"
    "stream\n"
    "BT /F1 24 Tf 20 100 Td (Page Two Text) Tj ET\n"
    "endstream\n"
    "endobj\n"
    "8 0 obj\n"
    "<< /Title (Test Document Title) /Author (Jane Q. Author) /Subject (A Test Subject) "
    "/Keywords (pdf, metadata, test) /Creator (AgentEngine Test Suite) "
    "/Producer (AgentEngine PDF Test Generator) /CreationDate (D:20260829120000Z) "
    "/ModDate (D:20260829130000Z) >>\n"
    "endobj\n"
    "xref\n"
    "0 9\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000058 00000 n \n"
    "0000000121 00000 n \n"
    "0000000247 00000 n \n"
    "0000000373 00000 n \n"
    "0000000443 00000 n \n"
    "0000000537 00000 n \n"
    "0000000631 00000 n \n"
    "trailer\n"
    "<< /Size 9 /Root 1 0 R /Info 8 0 R >>\n"
    "startxref\n"
    "904\n"
    "%%EOF";

// Same pattern as test_extract_pdf_text.cpp/test_extract_pdf_toc.cpp's own fake adapter -- returns
// canned bytes, no real disk I/O; ExtractPdfMetadata's own scratch-file write and worker invocation
// are what this test actually proves.
class FakePdfFileSystemAdapter final : public agentengine::FileSystemAdapter {
public:
    result<std::vector<std::byte>> read_file(std::string_view) override {
        std::string const text(kTestPdfWithInfo);
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

void test_real_end_to_end_metadata_extraction() {
    using agentengine::tools::ExtractPdfMetadata;

    agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
        {agentengine::cap::FsRead{"work", "", std::nullopt}});
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    FakePdfFileSystemAdapter adapter;
    ctx.sandbox_fs = &adapter;

    ExtractPdfMetadata::Args args;
    args.path = "meta_test.pdf";  // the fake adapter ignores this and returns kTestPdfWithInfo regardless

    auto reply = ExtractPdfMetadata::invoke(args, ctx);
    if (!reply.has_value()) {
        std::fprintf(stderr, "extract_pdf_metadata real end-to-end call failed: code=%s message=%s\n",
                     reply.error().code.c_str(), reply.error().message.c_str());
    }
    check(reply.has_value(), "real metadata genuinely extracts through the real sandboxed worker "
                              "end to end");
    if (reply) {
        check(reply->total_page_count == 2, "total_page_count matches the real 2-page document");
        check(reply->title.has_value() && *reply->title == "Test Document Title",
              "title matches exactly what the /Info dictionary set");
        check(reply->author.has_value() && *reply->author == "Jane Q. Author",
              "author matches exactly");
        check(reply->subject.has_value() && *reply->subject == "A Test Subject",
              "subject matches exactly");
        check(reply->keywords.has_value() && *reply->keywords == "pdf, metadata, test",
              "keywords matches exactly");
        check(reply->creator.has_value() && *reply->creator == "AgentEngine Test Suite",
              "creator matches exactly");
        check(reply->producer.has_value() && *reply->producer == "AgentEngine PDF Test Generator",
              "producer matches exactly");
        check(reply->creation_date.has_value() && *reply->creation_date == "D:20260829120000Z",
              "creation_date matches exactly");
        check(reply->mod_date.has_value() && *reply->mod_date == "D:20260829130000Z",
              "mod_date matches exactly");
    }
}

void test_ambiguous_source_rejected_before_any_sandbox_work() {
    using agentengine::tools::ExtractPdfMetadata;
    agentengine::CapabilitySet const held;
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    ExtractPdfMetadata::Args args;  // neither url nor path set
    auto reply = ExtractPdfMetadata::invoke(args, ctx);
    check(!reply.has_value(), "neither url nor path set -> rejected");
    if (!reply) {
        check(reply.error().code == "extract_pdf_metadata.ambiguous_source",
              "ambiguity uses its own error code");
    }
}

}  // namespace

int main() {
    test_parse_worker_output_happy_path_multiple_fields();
    test_parse_worker_output_field_pdfium_reports_empty_is_absent();
    test_parse_worker_output_truncated_trailing_record_stops();
    test_parse_worker_output_unknown_tag_index_rejected();
    test_parse_worker_output_no_meta_record_at_all_fails();
    test_parse_worker_output_only_page_count_no_tag_fields_at_all();
    test_ambiguous_source_rejected_before_any_sandbox_work();
    test_real_end_to_end_metadata_extraction();

    if (g_failures == 0) {
        std::printf("test_extract_pdf_metadata: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_extract_pdf_metadata: %d check(s) failed\n", g_failures);
    return 1;
}
