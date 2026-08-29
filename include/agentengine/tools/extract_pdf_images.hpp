#pragma once
// A third candidate in 009-Plugin-and-Extension-System.md §7's "Document extraction" catalog row --
// PDF embedded-image METADATA listing (position/dimensions/format, NOT bytes), reusing the exact
// same sandboxed `agentengine_pdf_worker` process `extract_pdf_text.hpp`/`extract_pdf_toc.hpp`
// already spawn (its own `images` mode, `pdf_worker_main.cpp`), the same url/path sourcing, and the
// same companion-skill pairing pattern -- this file `#include`s `extract_pdf_text.hpp` specifically
// to reuse `extract_pdf_text_detail::{scratch_dir, unique_scratch_filename, fetch_via_url,
// fetch_via_path, invoke_worker}` rather than re-deriving any of it, the same non-duplication
// rationale `extract_pdf_toc.hpp`'s own top comment already gives for this same sharing pattern.
//
// SCOPE -- METADATA ONLY, DELIBERATELY, NOT AN OVERSIGHT: this tool never returns raw image bytes.
// A single embedded image can easily be hundreds of KB to several MB -- fundamentally incompatible
// with the existing worker-output protocol (`exec()`'s stdout pipe is only drained AFTER the process
// exits, with a small ~768KB-1MB self-imposed ceiling -- see `pdf_worker_main.cpp`'s own top comment
// on `compute_output_ceiling()`). Returning real image bytes through that channel would either be
// silently truncated garbage or deadlock the worker. Doing it properly needs a NEW mechanism (the
// worker writing to a scratch file, the host reading that file and routing it through this
// codebase's real `blob_sink`/`BlobRef` promotion) that has not been designed or reviewed -- out of
// scope here. The worker side uses `FPDFImageObj_GetImageMetadata` (dimensions/format WITHOUT
// decoding the full bitmap), never `FPDFImageObj_GetBitmap`/`FPDFBitmap_GetBuffer`.
//
// OUTPUT: `pdf_worker_main.cpp`'s own `IMAGE_ENTRY` binary record kind (kind=3) -- [4-byte uint32
// page_index][4-byte uint32 width][4-byte uint32 height][4-byte uint32 bits_per_pixel], always 16
// bytes, no variable-length payload (images have no title the way TOC_ENTRY does). The record's
// outer `index` field is the sequential, contiguous 0-based image-entry number across the WHOLE
// document -- the same "must match the count already emitted" contiguity discipline PAGE/TOC_ENTRY
// records already use.
//
// No `total_entry_count`/`preview`+`blob` promotion the way `ExtractPdfText` has, for the same
// reason `ExtractPdfToc` doesn't: unlike `FPDF_GetPageCount()` (an O(1) query), PDFium has no
// equivalent O(1) "total embedded image count" -- finding out costs the same per-page object
// enumeration as emitting the entries, so the worker never claims to know a total ahead of time. A
// pathologically image-heavy document is still bounded -- the worker's own kMaxImageEntries/output-
// ceiling caps are the primary defense, and the generic tool_pipeline.hpp promotion (006 §7's own
// pipeline-level backstop, step 9) still applies to this Reply like any other tool's.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/skill_source.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/tools/extract_pdf_text.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::tools {

// ae-naming-lint: allow ExtractPdfImagesArgs — new tool, matches every other Args type's own naming
struct ExtractPdfImagesArgs {
    std::optional<std::string> url;
    std::optional<std::string> path;
};
AE_JSON_SCHEMA(ExtractPdfImagesArgs, url, path)

// ae-naming-lint: allow ImageEntry — new schema-bearing nested type, matches this tool's own naming
struct ImageEntry {
    std::uint32_t page_index = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bits_per_pixel = 0;
};
AE_JSON_SCHEMA(ImageEntry, page_index, width, height, bits_per_pixel)

// ae-naming-lint: allow ExtractPdfImagesReply — new tool, matches every other Reply type's own naming
struct ExtractPdfImagesReply {
    std::vector<ImageEntry> images;         // document order, exactly as pdf_worker_main.cpp emitted
    std::uint32_t images_processed = 0;     // == images.size(); named for symmetry with
                                             // ExtractPdfTextReply::pages_processed
    bool truncated = false;                 // true iff the worker's own cap (entry count or output
                                             // ceiling) stopped a listing that had more to give
};
AE_JSON_SCHEMA(ExtractPdfImagesReply, images, images_processed, truncated)

namespace extract_pdf_images_detail {

inline constexpr std::uint32_t kMagic = 0x31504541u;      // must match pdf_worker_main.cpp's kMagic
inline constexpr std::uint32_t kKindImageEntry = 3;        // must match pdf_worker_main.cpp's kKindImageEntry

// Parses pdf_worker_main.cpp's IMAGE_ENTRY record stream -- same "stop at the first structurally
// invalid or incomplete record" discipline ExtractPdfText/ExtractPdfToc's own parsers already use,
// for the same reason (a truncated trailing record from a capped/killed worker must never be
// miscounted). `truncated` is set whenever ANY record is dropped for being incomplete -- the only
// signal this side has that the worker's own cap fired, since (like TOC mode) there's no total to
// compare against.
[[nodiscard]] inline result<ExtractPdfImagesReply> parse_worker_output(std::string const& raw) {
    ExtractPdfImagesReply out;
    std::size_t offset = 0;
    while (offset + 16 <= raw.size()) {
        std::uint32_t header[4];
        std::memcpy(header, raw.data() + offset, sizeof(header));
        if (header[0] != kMagic) {
            out.truncated = true;
            break;
        }
        std::uint32_t const kind = header[1];
        std::uint32_t const index = header[2];
        std::uint32_t const payload_len = header[3];
        if (kind != kKindImageEntry) {
            out.truncated = true;
            break;
        }
        if (offset + 16 + payload_len > raw.size()) {
            out.truncated = true;  // incomplete trailing record -- discard, don't count
            break;
        }
        if (static_cast<std::size_t>(index) != out.images.size()) {
            out.truncated = true;  // non-contiguous -- stop rather than accept an out-of-order record
            break;
        }
        if (payload_len != 16) {
            out.truncated = true;  // wrong size to hold [page_index][width][height][bits_per_pixel]
            break;
        }

        std::uint32_t fields[4];
        std::memcpy(fields, raw.data() + offset + 16, 16);
        ImageEntry entry;
        entry.page_index = fields[0];
        entry.width = fields[1];
        entry.height = fields[2];
        entry.bits_per_pixel = fields[3];
        out.images.push_back(entry);

        offset += 16 + payload_len;
    }
    out.images_processed = static_cast<std::uint32_t>(out.images.size());
    return out;
}

}  // namespace extract_pdf_images_detail

// 009 §7's PDF embedded-image METADATA (not bytes) document-extraction candidate. Capability model
// identical to `ExtractPdfText`/`ExtractPdfToc` -- see `extract_pdf_text.hpp`'s own comment on why an
// empty static `Capabilities<>` ceiling plus a dynamic `find_net_out`/`find_fs_read` check is the
// right (and only) shape here too.
// ae-naming-lint: allow ExtractPdfImages — 009 §7's document-extraction candidate tool; 027 not yet updated
struct ExtractPdfImages
    : Tool<ExtractPdfImages, Capabilities<>, Approval<approval_mode::never_require>,
           EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "extract_pdf_images";
    static constexpr std::string_view description =
        "List the embedded images in a PDF under one of this run's granted network-egress hosts or "
        "sandbox paths. Set exactly one of 'url' or 'path'. Returns METADATA ONLY -- each image's "
        "page index, pixel width/height, and bits per pixel -- NEVER the image's own bytes/pixel "
        "content; there is currently no first-party way to retrieve the actual image content. Use "
        "this to discover what images a document contains and where, not to view them.";

    using Args = ExtractPdfImagesArgs;
    using Reply = ExtractPdfImagesReply;

    [[nodiscard]] static result<Reply> invoke(Args args, EffectContext& ctx) {
        return invoke_via(args, ctx, sandbox::HostEgressProxy{});
    }

    template <sandbox::NetEgressBackend Proxy>
    [[nodiscard]] static result<Reply> invoke_via(Args const& args, EffectContext& ctx,
                                                    Proxy const& proxy) {
        bool const has_url = args.url.has_value();
        bool const has_path = args.path.has_value();
        if (has_url == has_path) {
            return std::unexpected(error{failure_class::contract,
                                          "extract_pdf_images requires exactly one of 'url' or 'path'",
                                          "extract_pdf_images.ambiguous_source"});
        }

        auto bytes = has_url ? extract_pdf_text_detail::fetch_via_url(*args.url, ctx, proxy)
                              : extract_pdf_text_detail::fetch_via_path(*args.path, ctx);
        if (!bytes) return std::unexpected(bytes.error());

        std::filesystem::path const dir = extract_pdf_text_detail::scratch_dir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return std::unexpected(error{failure_class::resource,
                                          "failed to create the pdf scratch directory: " + ec.message(),
                                          "extract_pdf_images.scratch_dir_failed"});
        }
        std::filesystem::path const file = dir / extract_pdf_text_detail::unique_scratch_filename();
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) {
                return std::unexpected(error{failure_class::resource,
                                              "failed to open the pdf scratch file for writing",
                                              "extract_pdf_images.scratch_write_failed"});
            }
            out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
            if (!out) {
                return std::unexpected(error{failure_class::resource,
                                              "failed to write the pdf scratch file",
                                              "extract_pdf_images.scratch_write_failed"});
            }
        }

        auto raw_output = extract_pdf_text_detail::invoke_worker("images", file, ctx);
        std::filesystem::remove(file, ec);

        if (!raw_output) return std::unexpected(raw_output.error());

        return extract_pdf_images_detail::parse_worker_output(*raw_output);
    }
};

// Folded into the SAME companion skill as ExtractPdfText/ExtractPdfToc, not a third skill -- see
// extract_pdf_text.hpp's own comment on why one skill can name several tools (`allowed-tools` is a
// whitespace-separated list). Appends `extract_pdf_images` to that skill's `allowed-tools`/body
// rather than defining a second, overlapping skill -- see extract_pdf_text.hpp's own
// kExtractingDocumentTextSkillMd, which this candidate's text was written to be added alongside, not
// duplicated for.

}  // namespace agentengine::tools
