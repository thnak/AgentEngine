#pragma once
// A third candidate in 009-Plugin-and-Extension-System.md §7's "Document extraction" catalog row --
// PDF document-information-dictionary metadata (Title/Author/Subject/Keywords/Creator/Producer/
// CreationDate/ModDate, ISO 32000-1:2008 §14.3.3) plus the total page count, reusing the exact same
// sandboxed `agentengine_pdf_worker` process `extract_pdf_text.hpp` already spawns (its own `meta`
// mode, `pdf_worker_main.cpp`), the same url/path sourcing, and the same companion-skill pairing
// pattern -- this file `#include`s that one specifically to reuse `extract_pdf_text_detail::
// {scratch_dir, unique_scratch_filename, fetch_via_url, fetch_via_path, invoke_worker}` rather than
// re-deriving any of it. Same "authored together, in the same pass" sharing rationale
// `extract_pdf_toc.hpp`'s own top comment already gives -- this is not the S2 "share vs. duplicate"
// question `ReadContent`/`ExtractPdfText` faced.
//
// OUTPUT: `pdf_worker_main.cpp`'s own `META` (kind=0, total page count, already shared with
// `ExtractPdfText`) and `META_FIELD` (kind=3) binary record kinds -- a `META_FIELD` record's `index`
// identifies WHICH tag it carries, via the worker's own fixed mapping (`kMetaTagTable` there):
//
//   0=Title  1=Author  2=Subject  3=Keywords  4=Creator  5=Producer  6=CreationDate  7=ModDate
//
// Sparse: a tag the worker found empty/absent produces NO record at all, so `parse_worker_output`
// below leaves the corresponding `Reply` field unset (`std::optional`) rather than defaulting it to
// an empty string that would be indistinguishable from "PDFium reported an actually-empty value".

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/skill_source.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/tools/extract_pdf_text.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::tools {

// ae-naming-lint: allow ExtractPdfMetadataArgs — new tool, matches every other Args type's own naming
struct ExtractPdfMetadataArgs {
    std::optional<std::string> url;
    std::optional<std::string> path;
};
AE_JSON_SCHEMA(ExtractPdfMetadataArgs, url, path)

// ae-naming-lint: allow ExtractPdfMetadataReply — new tool, matches every other Reply type's own naming
struct ExtractPdfMetadataReply {
    std::optional<std::string> title;
    std::optional<std::string> author;
    std::optional<std::string> subject;
    std::optional<std::string> keywords;
    std::optional<std::string> creator;
    std::optional<std::string> producer;
    std::optional<std::string> creation_date;
    std::optional<std::string> mod_date;
    std::uint32_t total_page_count = 0;
};
AE_JSON_SCHEMA(ExtractPdfMetadataReply, title, author, subject, keywords, creator, producer,
               creation_date, mod_date, total_page_count)

namespace extract_pdf_metadata_detail {

inline constexpr std::uint32_t kMagic = 0x31504541u;      // must match pdf_worker_main.cpp's kMagic
inline constexpr std::uint32_t kKindMeta = 0;              // must match pdf_worker_main.cpp's kKindMeta
inline constexpr std::uint32_t kKindMetaField = 4;  // must match pdf_worker_main.cpp's kKindMetaField
                                                     // (bumped from 3 to 4 when merged with the
                                                     // concurrently-developed extract_pdf_images,
                                                     // which independently claimed kind=3 for its own
                                                     // IMAGE_ENTRY record kind first)

// Must match pdf_worker_main.cpp's own kMetaTagTable index mapping exactly (see that file's own top
// comment) -- a plain array of `std::optional<std::string>*` indexed by tag index would need a
// pointer-into-struct trick, so this instead just switches on index directly in parse_worker_output.
inline constexpr std::uint32_t kTagTitle = 0;
inline constexpr std::uint32_t kTagAuthor = 1;
inline constexpr std::uint32_t kTagSubject = 2;
inline constexpr std::uint32_t kTagKeywords = 3;
inline constexpr std::uint32_t kTagCreator = 4;
inline constexpr std::uint32_t kTagProducer = 5;
inline constexpr std::uint32_t kTagCreationDate = 6;
inline constexpr std::uint32_t kTagModDate = 7;

// Parses pdf_worker_main.cpp's META/META_FIELD record stream -- same "stop at the first structurally
// invalid or incomplete record, don't guess" discipline the sibling tools' own parsers already have:
// a magic mismatch, an out-of-range/unknown kind, an unknown tag index, or a truncated trailing
// record all stop parsing immediately rather than skipping past the bad record and continuing.
[[nodiscard]] inline result<ExtractPdfMetadataReply> parse_worker_output(std::string const& raw) {
    ExtractPdfMetadataReply out;
    bool got_meta = false;
    std::size_t offset = 0;
    while (offset + 16 <= raw.size()) {
        std::uint32_t header[4];
        std::memcpy(header, raw.data() + offset, sizeof(header));
        if (header[0] != kMagic) break;  // magic mismatch -- stop, don't guess
        std::uint32_t const kind = header[1];
        std::uint32_t const index = header[2];
        std::uint32_t const payload_len = header[3];
        if (offset + 16 + payload_len > raw.size()) break;  // incomplete trailing record -- discard

        if (kind == kKindMeta) {
            out.total_page_count = index;
            got_meta = true;
        } else if (kind == kKindMetaField) {
            std::string value = raw.substr(offset + 16, payload_len);
            switch (index) {
                case kTagTitle: out.title = std::move(value); break;
                case kTagAuthor: out.author = std::move(value); break;
                case kTagSubject: out.subject = std::move(value); break;
                case kTagKeywords: out.keywords = std::move(value); break;
                case kTagCreator: out.creator = std::move(value); break;
                case kTagProducer: out.producer = std::move(value); break;
                case kTagCreationDate: out.creation_date = std::move(value); break;
                case kTagModDate: out.mod_date = std::move(value); break;
                default: return std::unexpected(error{failure_class::transient,
                                                        "the pdf worker emitted a META_FIELD record "
                                                        "with an unknown tag index",
                                                        "extract_pdf_metadata.worker_output_malformed"});
            }
        } else {
            break;  // unknown kind -- stop rather than guess
        }
        offset += 16 + payload_len;
    }
    if (!got_meta) {
        return std::unexpected(error{failure_class::transient,
                                      "the pdf worker produced no META record (crashed before "
                                      "reporting a page count, or its output was corrupted)",
                                      "extract_pdf_metadata.worker_output_malformed"});
    }
    return out;
}

}  // namespace extract_pdf_metadata_detail

// 009 §7's PDF document-information-dictionary metadata candidate. Capability model identical to
// `ExtractPdfText`/`ExtractPdfToc` -- see `ExtractPdfText`'s own comment on why an empty static
// `Capabilities<>` ceiling plus a dynamic `find_net_out`/`find_fs_read` check is the right (and only)
// shape here too.
// ae-naming-lint: allow ExtractPdfMetadata — 009 §7's document-extraction candidate tool; 027 not yet updated
struct ExtractPdfMetadata
    : Tool<ExtractPdfMetadata, Capabilities<>, Approval<approval_mode::never_require>,
           EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "extract_pdf_metadata";
    static constexpr std::string_view description =
        "Read a PDF's document metadata (Title, Author, Subject, Keywords, Creator, Producer, "
        "CreationDate, ModDate) and total page count under one of this run's granted network-egress "
        "hosts or sandbox paths, without extracting any page text or outline. Set exactly one of "
        "'url' or 'path'. A field the PDF does not set is left unset, not returned as an empty "
        "string.";

    using Args = ExtractPdfMetadataArgs;
    using Reply = ExtractPdfMetadataReply;

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
                                          "extract_pdf_metadata requires exactly one of 'url' or 'path'",
                                          "extract_pdf_metadata.ambiguous_source"});
        }

        auto bytes = has_url ? extract_pdf_text_detail::fetch_via_url(*args.url, ctx, proxy)
                              : extract_pdf_text_detail::fetch_via_path(*args.path, ctx);
        if (!bytes) return std::unexpected(bytes.error());

        std::filesystem::path const dir = extract_pdf_text_detail::scratch_dir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return std::unexpected(
                error{failure_class::resource,
                      "failed to create the pdf scratch directory: " + ec.message(),
                      "extract_pdf_metadata.scratch_dir_failed"});
        }
        std::filesystem::path const file = dir / extract_pdf_text_detail::unique_scratch_filename();
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) {
                return std::unexpected(error{failure_class::resource,
                                              "failed to open the pdf scratch file for writing",
                                              "extract_pdf_metadata.scratch_write_failed"});
            }
            out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
            if (!out) {
                return std::unexpected(error{failure_class::resource,
                                              "failed to write the pdf scratch file",
                                              "extract_pdf_metadata.scratch_write_failed"});
            }
        }

        auto raw_output = extract_pdf_text_detail::invoke_worker("meta", file, ctx);
        std::filesystem::remove(file, ec);

        if (!raw_output) return std::unexpected(raw_output.error());

        return extract_pdf_metadata_detail::parse_worker_output(*raw_output);
    }
};

// Folded into the SAME companion skill as ExtractPdfText/ExtractPdfToc, not a third skill -- see
// extract_pdf_text.hpp's own kExtractingDocumentTextSkillMd, which this candidate's `allowed-tools`
// entry and body section were added to directly rather than duplicated for.

}  // namespace agentengine::tools
