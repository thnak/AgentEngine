// Proves tools/extract_pdf_toc.hpp's ExtractPdfToc tool -- 009 §7's PDF-outline document-extraction
// candidate, sharing extract_pdf_text.hpp's own sandboxed worker/sourcing infrastructure (its own
// file-top comment explains why this stays a shared, non-duplicated seam this time). Same two-tier
// shape as test_extract_pdf_text.cpp: pure-logic parse tests, then a REAL end-to-end test against a
// genuine, hand-verified-valid PDF with a real nested outline (byte-for-byte the same file this
// branch's own manual `agentengine_pdf_worker.exe toc <path>` run was checked against directly
// before this test was written -- confirmed to report three entries: "Chapter 1" (depth 0, page 0),
// "Section 1.1" (depth 1, page 0, nested under Chapter 1), "Chapter 2" (depth 0, page 1)).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "agentengine/tools/extract_pdf_toc.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using agentengine::result;
namespace toc_detail = agentengine::tools::extract_pdf_toc_detail;

// ---- Tier 1: pure-logic tests -------------------------------------------------------------------

std::string make_entry_record(std::uint32_t index, std::int32_t depth, std::int32_t page_index,
                               std::string const& title) {
    std::string payload;
    payload.resize(8);
    std::int32_t fields[2] = {depth, page_index};
    std::memcpy(payload.data(), fields, 8);
    payload += title;

    std::string out;
    out.resize(16);
    std::uint32_t header[4] = {toc_detail::kMagic, toc_detail::kKindTocEntry, index,
                                static_cast<std::uint32_t>(payload.size())};
    std::memcpy(out.data(), header, 16);
    out += payload;
    return out;
}

void test_parse_worker_output_happy_path() {
    std::string raw = make_entry_record(0, 0, 0, "Chapter 1");
    raw += make_entry_record(1, 1, 0, "Section 1.1");
    raw += make_entry_record(2, 0, 1, "Chapter 2");

    auto parsed = toc_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a well-formed TOC record stream parses");
    if (parsed) {
        check(!parsed->truncated, "a complete stream is not marked truncated");
        check(parsed->entries_processed == 3, "all three entries counted");
        check(parsed->entries.size() == 3, "all three entries captured");
        if (parsed->entries.size() == 3) {
            check(parsed->entries[0].depth == 0 && parsed->entries[0].title == "Chapter 1" &&
                      parsed->entries[0].page_index.has_value() && *parsed->entries[0].page_index == 0,
                  "entry 0 (Chapter 1) is exact: depth 0, page 0");
            check(parsed->entries[1].depth == 1 && parsed->entries[1].title == "Section 1.1" &&
                      parsed->entries[1].page_index.has_value() && *parsed->entries[1].page_index == 0,
                  "entry 1 (Section 1.1) is exact: depth 1 (nested), page 0");
            check(parsed->entries[2].depth == 0 && parsed->entries[2].title == "Chapter 2" &&
                      parsed->entries[2].page_index.has_value() && *parsed->entries[2].page_index == 1,
                  "entry 2 (Chapter 2) is exact: depth 0, page 1");
        }
    }
}

void test_parse_worker_output_no_destination_page_index_is_unset() {
    std::string raw = make_entry_record(0, 0, -1, "No destination");  // -1 sentinel, per the worker's
                                                                        // own real make_toc_entry_payload
    auto parsed = toc_detail::parse_worker_output(raw);
    check(parsed.has_value(), "parses");
    if (parsed && !parsed->entries.empty()) {
        check(!parsed->entries[0].page_index.has_value(),
              "the -1 sentinel maps to an UNSET page_index, not a garbage 0xFFFFFFFF value");
    }
}

void test_parse_worker_output_truncated_trailing_record_is_discarded() {
    std::string raw = make_entry_record(0, 0, 0, "complete entry");
    std::string partial_header(16, '\0');
    std::uint32_t header[4] = {toc_detail::kMagic, toc_detail::kKindTocEntry, 1, 9999};
    std::memcpy(partial_header.data(), header, 16);
    raw += partial_header;
    raw += "short";

    auto parsed = toc_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a truncated trailing record does not fail the whole parse");
    if (parsed) {
        check(parsed->truncated, "the truncation IS flagged -- this is the only signal this side has "
                                  "that the worker's own cap fired");
        check(parsed->entries.size() == 1, "only the complete entry is kept");
    }
}

void test_parse_worker_output_non_contiguous_index_stops() {
    std::string raw = make_entry_record(1, 0, 0, "skipped entry 0");  // index 1, but entries is empty
    auto parsed = toc_detail::parse_worker_output(raw);
    check(parsed.has_value(), "still parses");
    if (parsed) {
        check(parsed->entries.empty(), "a non-contiguous index stops parsing immediately");
        check(parsed->truncated, "flagged truncated -- a real stream never looks like this unless "
                                  "something went wrong");
    }
}

void test_parse_worker_output_empty_stream_is_zero_entries_not_an_error() {
    // A PDF with no outline at all -- pdf_worker_main.cpp's own run_toc_mode() returns 0 TOC_ENTRY
    // records for this case, not an error (see that function's own comment).
    auto parsed = toc_detail::parse_worker_output("");
    check(parsed.has_value(), "an empty stream (no outline) is a valid, empty result, not a failure");
    if (parsed) {
        check(parsed->entries.empty() && parsed->entries_processed == 0 && !parsed->truncated,
              "zero entries, not truncated -- a document can legitimately have no outline");
    }
}

}  // namespace

// ---- Tier 2: real end-to-end test ---------------------------------------------------------------

namespace {

// Byte-for-byte the same minimal, hand-verified-valid 2-page PDF (with a real, nested outline) this
// branch's own manual `agentengine_pdf_worker.exe toc` run was checked against directly before this
// test was written. Outline: "Chapter 1" (-> page 0) with child "Section 1.1" (-> page 0), then
// "Chapter 2" (-> page 1).
constexpr char const* kTestPdfWithToc =
    "%PDF-1.4\n"
    "1 0 obj\n"
    "<< /Type /Catalog /Pages 2 0 R /Outlines 8 0 R >>\n"
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
    "<< /Type /Outlines /First 9 0 R /Last 11 0 R /Count 3 >>\n"
    "endobj\n"
    "9 0 obj\n"
    "<< /Title (Chapter 1) /Parent 8 0 R /Next 11 0 R /First 10 0 R /Last 10 0 R /Count 1 "
    "/Dest [3 0 R /Fit] >>\n"
    "endobj\n"
    "10 0 obj\n"
    "<< /Title (Section 1.1) /Parent 9 0 R /Dest [3 0 R /Fit] >>\n"
    "endobj\n"
    "11 0 obj\n"
    "<< /Title (Chapter 2) /Parent 8 0 R /Prev 9 0 R /Dest [4 0 R /Fit] >>\n"
    "endobj\n"
    "xref\n"
    "0 12\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000074 00000 n \n"
    "0000000137 00000 n \n"
    "0000000263 00000 n \n"
    "0000000389 00000 n \n"
    "0000000459 00000 n \n"
    "0000000553 00000 n \n"
    "0000000647 00000 n \n"
    "0000000719 00000 n \n"
    "0000000841 00000 n \n"
    "0000000917 00000 n \n"
    "trailer\n"
    "<< /Size 12 /Root 1 0 R >>\n"
    "startxref\n"
    "1003\n"
    "%%EOF";

// Same pattern as test_extract_pdf_text.cpp's own FakePdfFileSystemAdapter -- returns canned bytes,
// no real disk I/O; ExtractPdfToc's own scratch-file write and worker invocation are what this test
// actually proves.
class FakePdfFileSystemAdapter final : public agentengine::FileSystemAdapter {
public:
    result<std::vector<std::byte>> read_file(std::string_view) override {
        std::string const text(kTestPdfWithToc);
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

void test_real_end_to_end_toc_extraction() {
    using agentengine::tools::ExtractPdfToc;

    agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
        {agentengine::cap::FsRead{"work", "", std::nullopt}});
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    FakePdfFileSystemAdapter adapter;
    ctx.sandbox_fs = &adapter;

    ExtractPdfToc::Args args;
    args.path = "toc_test.pdf";  // the fake adapter ignores this and returns kTestPdfWithToc regardless

    auto reply = ExtractPdfToc::invoke(args, ctx);
    if (!reply.has_value()) {
        std::fprintf(stderr, "extract_pdf_toc real end-to-end call failed: code=%s message=%s\n",
                     reply.error().code.c_str(), reply.error().message.c_str());
    }
    check(reply.has_value(), "a real outline genuinely extracts through the real sandboxed worker "
                              "end to end");
    if (reply) {
        check(!reply->truncated, "the whole (tiny) outline was processed");
        check(reply->entries_processed == 3, "exactly the three real bookmark entries");
        check(reply->entries.size() == 3, "entries.size() matches entries_processed");
        if (reply->entries.size() == 3) {
            check(reply->entries[0].title == "Chapter 1" && reply->entries[0].depth == 0 &&
                      reply->entries[0].page_index.has_value() && *reply->entries[0].page_index == 0,
                  "Chapter 1: real title, top-level depth, real destination page 0");
            check(reply->entries[1].title == "Section 1.1" && reply->entries[1].depth == 1 &&
                      reply->entries[1].page_index.has_value() && *reply->entries[1].page_index == 0,
                  "Section 1.1: real title, correctly nested one level under Chapter 1, same page 0");
            check(reply->entries[2].title == "Chapter 2" && reply->entries[2].depth == 0 &&
                      reply->entries[2].page_index.has_value() && *reply->entries[2].page_index == 1,
                  "Chapter 2: real title, back to top-level depth, real destination page 1");
        }
    }
}

void test_ambiguous_source_rejected_before_any_sandbox_work() {
    using agentengine::tools::ExtractPdfToc;
    agentengine::CapabilitySet const held;
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    ExtractPdfToc::Args args;  // neither url nor path set
    auto reply = ExtractPdfToc::invoke(args, ctx);
    check(!reply.has_value(), "neither url nor path set -> rejected");
    if (!reply) {
        check(reply.error().code == "extract_pdf_toc.ambiguous_source",
              "ambiguity uses its own error code");
    }
}

}  // namespace

int main() {
    test_parse_worker_output_happy_path();
    test_parse_worker_output_no_destination_page_index_is_unset();
    test_parse_worker_output_truncated_trailing_record_is_discarded();
    test_parse_worker_output_non_contiguous_index_stops();
    test_parse_worker_output_empty_stream_is_zero_entries_not_an_error();
    test_ambiguous_source_rejected_before_any_sandbox_work();
    test_real_end_to_end_toc_extraction();

    if (g_failures == 0) {
        std::printf("test_extract_pdf_toc: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_extract_pdf_toc: %d check(s) failed\n", g_failures);
    return 1;
}
