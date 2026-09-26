// Proves tools/extract_pdf_images.hpp's ExtractPdfImages tool -- 009 §7's PDF embedded-image
// METADATA (never bytes) document-extraction candidate, sharing extract_pdf_text.hpp's own
// sandboxed worker/sourcing infrastructure (its own file-top comment explains why this stays a
// shared, non-duplicated seam this time). Same two-tier shape as test_extract_pdf_toc.cpp: pure-
// logic parse tests, then a REAL end-to-end test against a genuine, hand-verified-valid PDF with a
// real embedded image XObject (byte-for-byte the same file this branch's own manual
// `agentengine_pdf_worker.exe images <path>` run was checked against directly before this test was
// written -- confirmed to report one image: page 0, width 2, height 2).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "agentengine/tools/extract_pdf_images.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

using agentengine::result;
namespace images_detail = agentengine::tools::extract_pdf_images_detail;

// ---- Tier 1: pure-logic tests -------------------------------------------------------------------

std::string make_image_record(std::uint32_t index, std::uint32_t page_index, std::uint32_t width,
                               std::uint32_t height, std::uint32_t bits_per_pixel) {
    std::string payload;
    payload.resize(16);
    std::uint32_t fields[4] = {page_index, width, height, bits_per_pixel};
    std::memcpy(payload.data(), fields, 16);

    std::string out;
    out.resize(16);
    std::uint32_t header[4] = {images_detail::kMagic, images_detail::kKindImageEntry, index,
                                static_cast<std::uint32_t>(payload.size())};
    std::memcpy(out.data(), header, 16);
    out += payload;
    return out;
}

void test_parse_worker_output_happy_path() {
    std::string raw = make_image_record(0, 0, 100, 200, 24);
    raw += make_image_record(1, 0, 50, 50, 8);
    raw += make_image_record(2, 1, 640, 480, 32);

    auto parsed = images_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a well-formed IMAGE_ENTRY record stream parses");
    if (parsed) {
        check(!parsed->truncated, "a complete stream is not marked truncated");
        check(parsed->images_processed == 3, "all three entries counted");
        check(parsed->images.size() == 3, "all three entries captured");
        if (parsed->images.size() == 3) {
            check(parsed->images[0].page_index == 0 && parsed->images[0].width == 100 &&
                      parsed->images[0].height == 200 && parsed->images[0].bits_per_pixel == 24,
                  "entry 0 is exact: page 0, 100x200, 24bpp");
            check(parsed->images[1].page_index == 0 && parsed->images[1].width == 50 &&
                      parsed->images[1].height == 50 && parsed->images[1].bits_per_pixel == 8,
                  "entry 1 is exact: page 0 (second image on same page), 50x50, 8bpp");
            check(parsed->images[2].page_index == 1 && parsed->images[2].width == 640 &&
                      parsed->images[2].height == 480 && parsed->images[2].bits_per_pixel == 32,
                  "entry 2 is exact: page 1, 640x480, 32bpp");
        }
    }
}

void test_parse_worker_output_truncated_trailing_record_is_discarded() {
    std::string raw = make_image_record(0, 0, 10, 10, 24);
    std::string partial_header(16, '\0');
    std::uint32_t header[4] = {images_detail::kMagic, images_detail::kKindImageEntry, 1, 16};
    std::memcpy(partial_header.data(), header, 16);
    raw += partial_header;
    raw += "short byte";  // fewer than the declared 16-byte payload

    auto parsed = images_detail::parse_worker_output(raw);
    check(parsed.has_value(), "a truncated trailing record does not fail the whole parse");
    if (parsed) {
        check(parsed->truncated, "the truncation IS flagged -- this is the only signal this side has "
                                  "that the worker's own cap fired");
        check(parsed->images.size() == 1, "only the complete entry is kept");
    }
}

void test_parse_worker_output_non_contiguous_index_stops() {
    std::string raw = make_image_record(1, 0, 10, 10, 24);  // index 1, but images is empty
    auto parsed = images_detail::parse_worker_output(raw);
    check(parsed.has_value(), "still parses");
    if (parsed) {
        check(parsed->images.empty(), "a non-contiguous index stops parsing immediately");
        check(parsed->truncated, "flagged truncated -- a real stream never looks like this unless "
                                  "something went wrong");
    }
}

void test_parse_worker_output_empty_stream_is_zero_entries_not_an_error() {
    // A PDF with no embedded images at all -- pdf_worker_main.cpp's own run_images_mode() returns 0
    // IMAGE_ENTRY records for this case, not an error (mirrors run_toc_mode's own no-outline case).
    auto parsed = images_detail::parse_worker_output("");
    check(parsed.has_value(), "an empty stream (no images) is a valid, empty result, not a failure");
    if (parsed) {
        check(parsed->images.empty() && parsed->images_processed == 0 && !parsed->truncated,
              "zero entries, not truncated -- a document can legitimately have no embedded images");
    }
}

void test_parse_worker_output_wrong_payload_size_stops() {
    // A structurally-valid header but a payload_len that doesn't match the fixed 16-byte
    // [page_index][width][height][bits_per_pixel] shape -- must never be silently mis-decoded.
    std::string out;
    out.resize(16);
    std::uint32_t header[4] = {images_detail::kMagic, images_detail::kKindImageEntry, 0, 8};
    std::memcpy(out.data(), header, 16);
    out += std::string(8, '\0');  // only 8 bytes of payload, not the required 16

    auto parsed = images_detail::parse_worker_output(out);
    check(parsed.has_value(), "still parses (returns an empty/truncated result, not an error)");
    if (parsed) {
        check(parsed->images.empty(), "a wrong-sized payload is rejected, not misread");
        check(parsed->truncated, "flagged truncated");
    }
}

}  // namespace

// ---- Tier 2: real end-to-end test ---------------------------------------------------------------

namespace {

// Byte-for-byte the same minimal, hand-verified-valid single-page PDF (with a real, uncompressed
// 2x2 DeviceRGB embedded image XObject painted onto the page via a `Do` operator) this branch's own
// manual `agentengine_pdf_worker.exe images` run was checked against directly before this test was
// written -- confirmed to report exactly one IMAGE_ENTRY: page_index=0, width=2, height=2,
// bits_per_pixel=24. Generated (and its xref byte offsets computed for real, not guessed) via a
// throwaway Python script, then verified against the real worker binary before being embedded here.
constexpr char kTestPdfWithImageBytes[] =
    "%PDF-1.4\n"
    "1 0 obj\n"
    "<< /Type /Catalog /Pages 2 0 R >>\n"
    "endobj\n"
    "2 0 obj\n"
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
    "endobj\n"
    "3 0 obj\n"
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << /XObject << /Im0 4 0 R >> >> "
    "/Contents 5 0 R >>\n"
    "endobj\n"
    "4 0 obj\n"
    "<< /Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8 "
    "/Length 12 >>\n"
    "stream\n"
    "\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff"
    "\nendstream\n"
    "endobj\n"
    "5 0 obj\n"
    "<< /Length 30 >>\n"
    "stream\n"
    "q 100 0 0 100 0 0 cm /Im0 Do Q\n"
    "endstream\n"
    "endobj\n"
    "xref\n"
    "0 6\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000058 00000 n \n"
    "0000000115 00000 n \n"
    "0000000245 00000 n \n"
    "0000000400 00000 n \n"
    "trailer\n"
    "<< /Size 6 /Root 1 0 R >>\n"
    "startxref\n"
    "480\n"
    "%%EOF";
constexpr std::size_t kTestPdfWithImageLen = sizeof(kTestPdfWithImageBytes) - 1;  // exclude the NUL

// Same pattern as test_extract_pdf_toc.cpp's own FakePdfFileSystemAdapter -- returns canned bytes,
// no real disk I/O; ExtractPdfImages's own scratch-file write and worker invocation are what this
// test actually proves. A plain C string literal cannot hold the embedded image's raw binary pixel
// bytes safely (some are 0x00), so this uses a fixed-size char array + explicit length instead of
// std::string(kTestPdfWithImageBytes), which would stop at the first embedded NUL.
class FakePdfFileSystemAdapter final : public agentengine::FileSystemAdapter {
public:
    result<std::vector<std::byte>> read_file(std::string_view) override {
        return std::vector<std::byte>(
            reinterpret_cast<std::byte const*>(kTestPdfWithImageBytes),
            reinterpret_cast<std::byte const*>(kTestPdfWithImageBytes) + kTestPdfWithImageLen);
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

void test_real_end_to_end_image_extraction() {
    using agentengine::tools::ExtractPdfImages;

    agentengine::CapabilitySet const held = agentengine::CapabilitySet::grant_root(
        {agentengine::cap::FsRead{"work", "", std::nullopt}});
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    FakePdfFileSystemAdapter adapter;
    ctx.sandbox_fs = &adapter;

    ExtractPdfImages::Args args;
    args.path = "images_test.pdf";  // the fake adapter ignores this and returns the canned PDF regardless

    auto reply = ExtractPdfImages::invoke(args, ctx);
    if (!reply.has_value()) {
        std::fprintf(stderr, "extract_pdf_images real end-to-end call failed: code=%s message=%s\n",
                     reply.error().code.c_str(), reply.error().message.c_str());
    }
    check(reply.has_value(), "a real embedded image genuinely extracts through the real sandboxed "
                              "worker end to end");
    if (reply) {
        check(!reply->truncated, "the whole (tiny) document was processed");
        check(reply->images_processed == 1, "exactly the one real embedded image");
        check(reply->images.size() == 1, "images.size() matches images_processed");
        if (reply->images.size() == 1) {
            check(reply->images[0].page_index == 0, "the image is on page 0, the only page");
            check(reply->images[0].width == 2, "real width from FPDFImageObj_GetImageMetadata: 2");
            check(reply->images[0].height == 2, "real height from FPDFImageObj_GetImageMetadata: 2");
            check(reply->images[0].bits_per_pixel > 0,
                  "a real, nonzero bits_per_pixel was reported (DeviceRGB @ 8 bpc)");
        }
    }
}

void test_ambiguous_source_rejected_before_any_sandbox_work() {
    using agentengine::tools::ExtractPdfImages;
    agentengine::CapabilitySet const held;
    agentengine::EffectContext ctx;
    ctx.principal = agentengine::Principal{"test-principal", ""};
    ctx.capabilities = agentengine::borrow_capabilities(held);

    ExtractPdfImages::Args args;  // neither url nor path set
    auto reply = ExtractPdfImages::invoke(args, ctx);
    check(!reply.has_value(), "neither url nor path set -> rejected");
    if (!reply) {
        check(reply.error().code == "extract_pdf_images.ambiguous_source",
              "ambiguity uses its own error code");
    }
}

}  // namespace

int main() {
    test_parse_worker_output_happy_path();
    test_parse_worker_output_truncated_trailing_record_is_discarded();
    test_parse_worker_output_non_contiguous_index_stops();
    test_parse_worker_output_empty_stream_is_zero_entries_not_an_error();
    test_parse_worker_output_wrong_payload_size_stops();
    test_ambiguous_source_rejected_before_any_sandbox_work();
    test_real_end_to_end_image_extraction();

    if (g_failures == 0) {
        std::printf("test_extract_pdf_images: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_extract_pdf_images: %d check(s) failed\n", g_failures);
    return 1;
}
