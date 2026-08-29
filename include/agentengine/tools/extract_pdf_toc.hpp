#pragma once
// A second candidate in 009-Plugin-and-Extension-System.md §7's "Document extraction" catalog row --
// PDF table-of-contents/outline extraction, reusing the exact same sandboxed `agentengine_pdf_worker`
// process `extract_pdf_text.hpp` already spawns (its own `toc` mode, `pdf_worker_main.cpp`), the same
// url/path sourcing, and the same companion-skill pairing pattern -- this file `#include`s that one
// specifically to reuse `extract_pdf_text_detail::{scratch_dir, unique_scratch_filename, fetch_via_
// url, fetch_via_path, invoke_worker}` rather than re-deriving any of it. This is NOT the S2 "share
// vs. duplicate" question `ReadContent`/`ExtractPdfText` faced -- both this file and
// `extract_pdf_text.hpp` are authored together, in the same pass, so sharing carries none of that
// question's retrofit-an-already-shipped-contract cost.
//
// OUTPUT: `pdf_worker_main.cpp`'s own `TOC_ENTRY` binary record kind (kind=2) -- [4-byte int32
// depth][4-byte int32 page_index, -1 if none][UTF-8 title]. Records arrive in ISO 32000-1:2008
// §12.3.3's own pre-order document-outline order (parent, then children, then next sibling) --
// `entries` below preserves that order exactly, so a caller can render an indented outline directly
// from `depth` without any further sorting.
//
// No `total_entry_count`/`preview`+`blob` promotion the way `ExtractPdfText` has: unlike
// `FPDF_GetPageCount()` (an O(1) query), PDFium has no equivalent O(1) "total bookmark count" --
// finding out costs the same traversal as emitting the entries, so the worker never claims to know a
// total ahead of time (see pdf_worker_main.cpp's own top comment). A pathologically large outline is
// still bounded -- the worker's own kMaxTocEntries/output-ceiling caps (S4) are the primary defense,
// and the generic tool_pipeline.hpp promotion (006 §7's own pipeline-level backstop, step 9) still
// applies to this Reply like any other tool's, so an oversized result is never silently inlined --
// this file just doesn't ALSO hand-rolls its own preview/blob logic the way the read_content-class
// candidate does, since a title list is a structurally different shape than one long text blob.

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

// ae-naming-lint: allow ExtractPdfTocArgs — new tool, matches every other Args type's own naming
struct ExtractPdfTocArgs {
    std::optional<std::string> url;
    std::optional<std::string> path;
};
AE_JSON_SCHEMA(ExtractPdfTocArgs, url, path)

// ae-naming-lint: allow TocEntry — new schema-bearing nested type, matches this tool's own naming
struct TocEntry {
    std::uint32_t depth = 0;
    std::optional<std::uint32_t> page_index;  // unset iff the bookmark has no destination
    std::string title;
};
AE_JSON_SCHEMA(TocEntry, depth, page_index, title)

// ae-naming-lint: allow ExtractPdfTocReply — new tool, matches every other Reply type's own naming
struct ExtractPdfTocReply {
    std::vector<TocEntry> entries;         // pre-order, exactly as pdf_worker_main.cpp emitted them
    std::uint32_t entries_processed = 0;   // == entries.size(); named explicitly for symmetry with
                                            // ExtractPdfTextReply::pages_processed
    bool truncated = false;                // true iff the worker's own cap (entry count or output
                                            // ceiling) stopped a traversal that had more to give
};
AE_JSON_SCHEMA(ExtractPdfTocReply, entries, entries_processed, truncated)

namespace extract_pdf_toc_detail {

inline constexpr std::uint32_t kMagic = 0x31504541u;  // must match pdf_worker_main.cpp's kMagic
inline constexpr std::uint32_t kKindTocEntry = 2;      // must match pdf_worker_main.cpp's kKindTocEntry

// Parses pdf_worker_main.cpp's TOC_ENTRY record stream -- same "stop at the first structurally
// invalid or incomplete record" discipline as ReadPdfText's own parse_worker_output, for the same
// reason (a truncated trailing record from a capped/killed worker must never be miscounted).
// `truncated` is set whenever ANY record is dropped for being incomplete -- the only signal this
// side has that the worker's own cap fired, since (unlike page mode) there's no total to compare
// against.
[[nodiscard]] inline result<ExtractPdfTocReply> parse_worker_output(std::string const& raw) {
    ExtractPdfTocReply out;
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
        if (kind != kKindTocEntry) {
            out.truncated = true;
            break;
        }
        if (offset + 16 + payload_len > raw.size()) {
            out.truncated = true;  // incomplete trailing record -- discard, don't count
            break;
        }
        if (static_cast<std::size_t>(index) != out.entries.size()) {
            out.truncated = true;  // non-contiguous -- stop rather than accept an out-of-order record
            break;
        }
        if (payload_len < 8) {
            out.truncated = true;  // too short to even hold [depth][page_index]
            break;
        }

        std::int32_t fields[2];
        std::memcpy(fields, raw.data() + offset + 16, 8);
        TocEntry entry;
        entry.depth = static_cast<std::uint32_t>(fields[0]);
        if (fields[1] >= 0) entry.page_index = static_cast<std::uint32_t>(fields[1]);
        entry.title = raw.substr(offset + 16 + 8, payload_len - 8);
        out.entries.push_back(std::move(entry));

        offset += 16 + payload_len;
    }
    out.entries_processed = static_cast<std::uint32_t>(out.entries.size());
    return out;
}

}  // namespace extract_pdf_toc_detail

// 009 §7's PDF outline/table-of-contents candidate. Capability model identical to `ExtractPdfText`
// -- see that file's own comment on why an empty static `Capabilities<>` ceiling plus a dynamic
// `find_net_out`/`find_fs_read` check is the right (and only) shape here too.
// ae-naming-lint: allow ExtractPdfToc — 009 §7's document-extraction candidate tool; 027 not yet updated
struct ExtractPdfToc : Tool<ExtractPdfToc, Capabilities<>, Approval<approval_mode::never_require>,
                             EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "extract_pdf_toc";
    static constexpr std::string_view description =
        "Extract the table of contents (bookmark outline) from a PDF under one of this run's "
        "granted network-egress hosts or sandbox paths. Set exactly one of 'url' or 'path'. Returns "
        "entries in document order with a 0-based nesting depth and an optional destination page "
        "index -- not the page text itself (use extract_pdf_text for that).";

    using Args = ExtractPdfTocArgs;
    using Reply = ExtractPdfTocReply;

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
                                          "extract_pdf_toc requires exactly one of 'url' or 'path'",
                                          "extract_pdf_toc.ambiguous_source"});
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
                                          "extract_pdf_toc.scratch_dir_failed"});
        }
        std::filesystem::path const file = dir / extract_pdf_text_detail::unique_scratch_filename();
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) {
                return std::unexpected(error{failure_class::resource,
                                              "failed to open the pdf scratch file for writing",
                                              "extract_pdf_toc.scratch_write_failed"});
            }
            out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
            if (!out) {
                return std::unexpected(error{failure_class::resource,
                                              "failed to write the pdf scratch file",
                                              "extract_pdf_toc.scratch_write_failed"});
            }
        }

        auto raw_output = extract_pdf_text_detail::invoke_worker("toc", file, ctx);
        std::filesystem::remove(file, ec);

        if (!raw_output) return std::unexpected(raw_output.error());

        return extract_pdf_toc_detail::parse_worker_output(*raw_output);
    }
};

// Folded into the SAME companion skill as ExtractPdfText, not a second skill -- one skill can name
// several tools (`allowed-tools` is a list, per its own already-real, already-multi-value use in
// this codebase's skill format), and both tools teach the same underlying idea ("this engine can
// read structured PDF content for you; don't fetch raw bytes and parse them yourself"). Appends
// `extract_pdf_toc` to the skill's `allowed-tools`/body rather than defining a second, overlapping
// skill -- see extract_pdf_text.hpp's own kExtractingDocumentTextSkillMd, which this candidate's
// text was written to be added alongside, not duplicated for.

}  // namespace agentengine::tools
