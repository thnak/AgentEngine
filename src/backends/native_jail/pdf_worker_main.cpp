// The sandboxed PDF text-extraction worker (docs/planning/pdf-text-extraction-design-draft.md
// Revision 7, S3a). Spawned per call via NativeJailBackend::create()/exec() (Windows) or
// LinuxNativeJailBackend::create()/exec() (Linux) -- never linked into the host `agentengine`
// process itself; this is the whole point of S3a's isolation posture (a PDFium memory-corruption
// bug now runs inside a throwaway, resource-capped child, never the address space holding every
// session's CapabilitySet/BoundCapability state).
//
// argv[1] is the mode ("text", "toc", "images", or "meta"); argv[2] is a path to a PDF file already
// placed by the host into a scratch directory it granted this process read access to (NativeJailBackend::
// grant_ro_path_once() on Windows; an ordinary SandboxSpec.mounts read-only bind on Linux -- S3a(ii)
// explains why the two platforms differ here). This binary trusts nothing about the PDF's CONTENTS
// (untrusted input, PDFium's own job to parse safely) but the PATH itself is host-controlled, never
// derived from anything guest/model-supplied directly (I2 -- the host already ran its own capability
// check before writing the scratch file).
//
// Output: a simple, ROBUST binary framing on stdout -- NOT the design draft's own illustrative
// text-delimiter sketch (`\x01PAGE\x01<n>\x01...`), which is unsafe against PDF text/titles that can
// themselves contain arbitrary bytes including 0x01. Each record is a fixed 16-byte little-endian
// header followed by `payload_len` bytes of payload:
//
//   [4] magic ('A','E','P','1')   [4] kind (0=META, 1=PAGE, 2=TOC_ENTRY, 3=IMAGE_ENTRY, 4=META_FIELD)
//   [4] index (META: total page count; PAGE: 0-based page index; TOC_ENTRY/IMAGE_ENTRY: 0-based,
//              contiguous entry sequence number across the WHOLE document -- the SAME "must match
//              the count already emitted" contiguity discipline PAGE records already use;
//              META_FIELD: a fixed tag index, see below)
//   [4] payload_len (bytes following; 0 for META, always 16 for IMAGE_ENTRY)
//   [payload_len] UTF-8 page text (PAGE), or [4-byte int32 depth][4-byte int32 page_index, -1 if
//              none][remaining bytes: UTF-8 title] (TOC_ENTRY), or [4-byte uint32 page_index]
//              [4-byte uint32 width][4-byte uint32 height][4-byte uint32 bits_per_pixel]
//              (IMAGE_ENTRY -- metadata only, from FPDFImageObj_GetImageMetadata; never the image's
//              own decoded pixel bytes, see this worker's own images-mode comment below for why),
//              or UTF-8 tag value (META_FIELD)
//
// TOC mode never emits a META record -- unlike FPDF_GetPageCount() (an O(1) query), PDFium has no
// equivalent O(1) "total bookmark count" -- finding out would cost the same traversal as emitting
// the entries themselves, so this worker just emits TOC_ENTRY records directly; the host learns
// "how many" from how many contiguous entries actually arrived. Images mode is the same: no META,
// for the same reason (a page's image count is only known by actually enumerating its objects).
//
// META mode (kind=4, META_FIELD) reads the PDF's document information dictionary (ISO 32000-1:2008
// §14.3.3's "Document Information Dictionary", the SAME 8 standard tags FPDF_GetMetaText's own doc
// comment names) via one FPDF_GetMetaText() call per tag, plus a single kKindMeta (0) record carrying
// the total page count -- the exact same O(1) FPDF_GetPageCount() call `run_text_mode` already makes,
// reused here rather than re-derived, so metadata mode gets a page count "for free" via an
// already-proven, already-shared record kind. The `index` field on a META_FIELD record identifies
// WHICH tag it carries, via a fixed mapping this worker owns (`kMetaTagTable` below):
//
//   0=Title  1=Author  2=Subject  3=Keywords  4=Creator  5=Producer  6=CreationDate  7=ModDate
//
// Sparse output: a tag PDFium reports as empty/absent gets NO record at all (not an empty-payload
// record) -- the host side leaves that field unset rather than treating "" as a real value.
//
// Never initializes PDFium's FormFill/V8 engine (round-1 red-team's own mitigating-fact finding:
// plain FPDFText_* extraction does not execute embedded PDF JavaScript) -- only FPDF_InitLibrary(),
// FPDF_LoadDocument(), FPDF_LoadPage(), and the FPDFText_*/FPDFPage_*/FPDFPageObj_*/FPDFImageObj_*
// (metadata-only, never FPDFImageObj_GetBitmap) extraction APIs are ever called.
//
// Output-byte ceiling (S3a/S4 item 5): `exec()` drains stdout only AFTER this process exits
// (native_jail_backend.cpp's own documented, intentional architecture) with a fixed pipe buffer --
// writing more than that buffer holds would block this process in write() forever, since nothing
// reads the pipe until it already exited. This worker self-limits its OWN cumulative output well
// under that buffer and exits cleanly (not killed) once reached, rather than blocking. On Windows,
// CreatePipe's requested size is a real floor (not a hint) -- a fixed constant, safely under the
// 1 MiB `native_jail_backend.cpp` always requests, is sound. On Linux, `native_jail_backend.cpp`'s
// own F_SETPIPE_SZ enlargement is best-effort and silently falls back to a much smaller OS default
// under a real, plausible sysctl configuration -- this worker queries its OWN stdout pipe directly
// via F_GETPIPE_SZ (valid on either end of a pipe since Linux 2.6.35) rather than trusting a fixed
// assumption, per round-6 red-team's own suggested fix for that exact false claim.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "fpdf_doc.h"
#include "fpdf_edit.h"
#include "fpdf_text.h"
#include "fpdfview.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uint32_t kMagic = 0x31504541u;  // 'A','E','P','1' as a little-endian uint32
constexpr std::uint32_t kKindMeta = 0;
constexpr std::uint32_t kKindPage = 1;
constexpr std::uint32_t kKindTocEntry = 2;
constexpr std::uint32_t kKindImageEntry = 3;
constexpr std::uint32_t kKindMetaField = 4;

// Fixed tag-index mapping for META_FIELD records -- see this file's own top comment. Order here
// fixes the `index` value each tag maps to; do not reorder without also updating the top comment and
// the host-side parser (extract_pdf_metadata.hpp).
constexpr struct { std::uint32_t index; char const* tag; } kMetaTagTable[] = {
    {0, "Title"},    {1, "Author"},   {2, "Subject"},      {3, "Keywords"},
    {4, "Creator"},  {5, "Producer"}, {6, "CreationDate"}, {7, "ModDate"},
};

// Cycle/DoS guards for the outline traversal (PDFDium's own FPDFBookmark_GetNextSibling() doc
// comment: "the caller is responsible for handling circular bookmark references, as may arise from
// malformed documents"). exec()'s own wall_ms is the real backstop either way, but these bound the
// traversal itself rather than relying solely on an external kill for an otherwise-ordinary-looking
// malformed-outline input.
constexpr int kMaxTocEntries = 5000;
constexpr int kMaxTocDepth = 32;

// Same discipline for image enumeration: a hostile document can pack in thousands of tiny/degenerate
// image XObjects across its pages purely to make a host iterate forever. exec()'s own wall_ms is
// still the real backstop, but this bounds the total number of IMAGE_ENTRY records this worker will
// ever emit, the same way kMaxTocEntries bounds the outline traversal above.
constexpr int kMaxImageEntries = 2000;

#if defined(_WIN32)
// CreatePipe's requested size is a real floor on Windows (not a hint) -- native_jail_backend.cpp's
// exec() always requests 1 MiB (kPipeBufferBytes); this constant leaves real margin under that for
// this worker's own framing overhead and whatever else is already in flight in the pipe.
constexpr std::size_t kOutputCeilingBytes = 768ull * 1024;
#else
// Best-effort measured margin, computed at startup below (F_GETPIPE_SZ) -- this is only the
// fallback for when that query itself fails, sized conservatively under Linux's own documented
// ~64 KiB default pipe capacity so even an un-enlarged pipe stays safe.
constexpr std::size_t kFallbackOutputCeilingBytes = 48ull * 1024;
#endif

[[nodiscard]] std::size_t compute_output_ceiling() {
#if defined(_WIN32)
    return kOutputCeilingBytes;
#else
    long const real_size = ::fcntl(STDOUT_FILENO, F_GETPIPE_SZ);
    if (real_size <= 0) return kFallbackOutputCeilingBytes;
    // Three-quarters of the REAL negotiated size -- real margin for this worker's own 16-byte
    // record headers and whatever slack the kernel/host side needs, not the whole buffer.
    return static_cast<std::size_t>(real_size) * 3 / 4;
#endif
}

// UCS-2 (PDFium's own term for what FPDFText_GetText fills -- BMP-only per that function's own
// doc comment: "ignores characters without UCS-2 representations") to UTF-8. Handles a lone
// surrogate defensively (replaced with U+FFFD) even though PDFium's own contract says it shouldn't
// emit one, rather than assuming that contract can never change or be misread.
[[nodiscard]] std::string ucs2_to_utf8(unsigned short const* units, int count) {
    std::string out;
    out.reserve(static_cast<std::size_t>(count) * 3 / 2);
    for (int i = 0; i < count; ++i) {
        unsigned int cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // lone surrogate -- not expected from FPDFText_GetText, replaced not trusted
        }
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Writes one record (header + payload) directly to stdout as raw bytes -- fwrite/fflush, not
// operator<<, so the exact byte count written is unambiguous and no locale/formatting layer can
// alter it. Returns false on any short write (a real I/O failure -- the process exits nonzero).
[[nodiscard]] bool write_record(std::uint32_t kind, std::uint32_t index, std::string const& payload) {
    std::uint32_t header[4] = {kMagic, kind, index, static_cast<std::uint32_t>(payload.size())};
    if (std::fwrite(header, sizeof(header), 1, stdout) != 1) return false;
    if (!payload.empty() && std::fwrite(payload.data(), 1, payload.size(), stdout) != payload.size()) {
        return false;
    }
    return std::fflush(stdout) == 0;
}

// Same UCS-2-to-UTF-8 contract as FPDFText_GetText (see ucs2_to_utf8 above), but for a bookmark's
// title: FPDFBookmark_GetTitle fills a UTF-16LE buffer and returns the byte count INCLUDING the
// terminating NUL, not a character count -- a real, easy-to-get-wrong difference from FPDFText_
// GetText's own contract, handled once here rather than at each call site.
[[nodiscard]] std::string bookmark_title_utf8(FPDF_BOOKMARK bookmark) {
    unsigned long const byte_len = FPDFBookmark_GetTitle(bookmark, nullptr, 0);
    if (byte_len <= 2) return {};  // 0, or just the 2-byte NUL terminator -- no real title
    std::vector<unsigned short> buf(byte_len / 2);
    FPDFBookmark_GetTitle(bookmark, buf.data(), byte_len);
    // buf's last unit is the NUL terminator -- exclude it, same "-1" ucs2_to_utf8's own PAGE-text
    // caller already applies for FPDFText_GetText's own trailing-terminator convention.
    int const unit_count = static_cast<int>(buf.size()) - 1;
    return unit_count > 0 ? ucs2_to_utf8(buf.data(), unit_count) : std::string{};
}

// Same UCS-2-to-UTF-8 contract as bookmark_title_utf8 above: FPDF_GetMetaText fills a UTF-16LE
// buffer and returns the byte count INCLUDING the terminating NUL, not a character count (FPDF_
// GetMetaText's own doc comment, fpdf_doc.h -- the SAME contract FPDFBookmark_GetTitle has, verified
// by re-reading both doc comments directly rather than assumed identical without checking). A
// parallel helper rather than a shared one -- the two PDFium calls have different signatures
// (FPDF_DOCUMENT+tag vs. FPDF_BOOKMARK) and this keeps each call site trivial to read.
[[nodiscard]] std::string meta_text_utf8(FPDF_DOCUMENT doc, char const* tag) {
    unsigned long const byte_len = FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (byte_len <= 2) return {};  // 0, or just the 2-byte NUL terminator -- no real value
    std::vector<unsigned short> buf(byte_len / 2);
    FPDF_GetMetaText(doc, tag, buf.data(), byte_len);
    // buf's last unit is the NUL terminator -- exclude it, same convention bookmark_title_utf8 and
    // the PAGE-text caller both already apply for their own respective trailing-terminator units.
    int const unit_count = static_cast<int>(buf.size()) - 1;
    return unit_count > 0 ? ucs2_to_utf8(buf.data(), unit_count) : std::string{};
}

// Binary TOC_ENTRY payload: [int32 depth][int32 page_index, -1 if none][UTF-8 title bytes].
[[nodiscard]] std::string make_toc_entry_payload(int depth, int page_index, std::string const& title) {
    std::string out;
    out.resize(8);
    std::int32_t const fields[2] = {static_cast<std::int32_t>(depth), static_cast<std::int32_t>(page_index)};
    std::memcpy(out.data(), fields, 8);
    out += title;
    return out;
}

// Recursive pre-order traversal (parent, then children, then next sibling) -- ISO 32000-1:2008
// §12.3.3's own document-outline order, matching how a reader's own sidebar shows bookmarks.
// `entries_emitted`/`bytes_written` are threaded through by reference so the SAME output-ceiling
// and entry-count guards apply across the whole tree, not per-branch. Returns false only on a real
// write failure (the caller then aborts with exit code 3, same as the text-mode loop).
[[nodiscard]] bool emit_toc_subtree(FPDF_DOCUMENT doc, FPDF_BOOKMARK bookmark, int depth,
                                     int& entries_emitted, std::size_t& bytes_written,
                                     std::size_t ceiling) {
    for (FPDF_BOOKMARK node = bookmark; node != nullptr;
         node = FPDFBookmark_GetNextSibling(doc, node)) {
        if (entries_emitted >= kMaxTocEntries) return true;  // cap reached -- stop, not an error

        int page_index = -1;
        if (FPDF_DEST const dest = FPDFBookmark_GetDest(doc, node); dest != nullptr) {
            page_index = FPDFDest_GetDestPageIndex(doc, dest);
        }
        std::string const payload =
            make_toc_entry_payload(depth, page_index, bookmark_title_utf8(node));

        std::size_t const record_bytes = 16 + payload.size();
        if (bytes_written + record_bytes > ceiling) return true;  // stop before writing -- never partial

        if (!write_record(kKindTocEntry, static_cast<std::uint32_t>(entries_emitted), payload)) {
            return false;
        }
        bytes_written += record_bytes;
        ++entries_emitted;

        if (depth < kMaxTocDepth) {
            if (FPDF_BOOKMARK const child = FPDFBookmark_GetFirstChild(doc, node); child != nullptr) {
                if (!emit_toc_subtree(doc, child, depth + 1, entries_emitted, bytes_written, ceiling)) {
                    return false;
                }
                if (entries_emitted >= kMaxTocEntries) return true;
            }
        }
    }
    return true;
}

[[nodiscard]] int run_text_mode(FPDF_DOCUMENT doc, std::size_t ceiling) {
    std::size_t bytes_written = 0;

    int const total_pages = FPDF_GetPageCount(doc);
    if (!write_record(kKindMeta, static_cast<std::uint32_t>(total_pages < 0 ? 0 : total_pages), {})) {
        return 3;
    }
    bytes_written += 16;  // the META record's own header

    for (int i = 0; i < total_pages; ++i) {
        FPDF_PAGE const page = FPDF_LoadPage(doc, i);
        if (page == nullptr) break;  // a corrupt page -- stop here, fail-visible via pages_processed

        FPDF_TEXTPAGE const text_page = FPDFText_LoadPage(page);
        if (text_page == nullptr) {
            FPDF_ClosePage(page);
            break;
        }

        int const char_count = FPDFText_CountChars(text_page);
        std::string utf8_text;
        if (char_count > 0) {
            std::vector<unsigned short> buf(static_cast<std::size_t>(char_count) + 1);
            int const written = FPDFText_GetText(text_page, 0, char_count, buf.data());
            // `written` includes the trailing NUL terminator per FPDFText_GetText's own contract.
            int const real_chars = written > 0 ? written - 1 : 0;
            utf8_text = ucs2_to_utf8(buf.data(), real_chars);
        }

        FPDFText_ClosePage(text_page);
        FPDF_ClosePage(page);

        std::size_t const record_bytes = 16 + utf8_text.size();
        if (bytes_written + record_bytes > ceiling) break;  // stop BEFORE writing -- never a partial record

        if (!write_record(kKindPage, static_cast<std::uint32_t>(i), utf8_text)) return 3;
        bytes_written += record_bytes;
    }
    return 0;
}

[[nodiscard]] int run_toc_mode(FPDF_DOCUMENT doc, std::size_t ceiling) {
    FPDF_BOOKMARK const root = FPDFBookmark_GetFirstChild(doc, nullptr);
    if (root == nullptr) return 0;  // no outline at all -- zero TOC_ENTRY records, not an error
    int entries_emitted = 0;
    std::size_t bytes_written = 0;
    return emit_toc_subtree(doc, root, 0, entries_emitted, bytes_written, ceiling) ? 0 : 3;
}

// Binary IMAGE_ENTRY payload: [uint32 page_index][uint32 width][uint32 height][uint32 bits_per_pixel]
// -- always exactly 16 bytes, no variable-length title (images have none). Metadata only, via
// FPDFImageObj_GetImageMetadata -- NEVER FPDFImageObj_GetBitmap/FPDFBitmap_GetBuffer, which would
// decode the full pixel buffer (can be hundreds of KB to several MB) and is fundamentally
// incompatible with this worker's own output-byte-ceiling discipline (see this file's own top
// comment); a real image's raw bytes need a scratch-file/BlobRef mechanism that does not exist yet,
// deliberately out of scope for this metadata-only listing.
[[nodiscard]] std::string make_image_entry_payload(std::uint32_t page_index, std::uint32_t width,
                                                     std::uint32_t height,
                                                     std::uint32_t bits_per_pixel) {
    std::string out;
    out.resize(16);
    std::uint32_t const fields[4] = {page_index, width, height, bits_per_pixel};
    std::memcpy(out.data(), fields, 16);
    return out;
}

// Loops pages (bounded by FPDF_GetPageCount, same as run_text_mode), enumerates each page's objects
// via FPDFPage_CountObjects/FPDFPage_GetObject, filters to FPDF_PAGEOBJ_IMAGE, and emits one
// IMAGE_ENTRY record per image found via FPDFImageObj_GetImageMetadata -- the metadata-only path
// that does NOT decode the full bitmap. Mirrors run_toc_mode's own shape: a total-entry cap
// (kMaxImageEntries) AND the output-ceiling check (bytes_written + record_bytes > ceiling), stopping
// cleanly -- not erroring -- once either is hit.
[[nodiscard]] int run_images_mode(FPDF_DOCUMENT doc, std::size_t ceiling) {
    int const total_pages = FPDF_GetPageCount(doc);
    int entries_emitted = 0;
    std::size_t bytes_written = 0;

    for (int page_index = 0; page_index < total_pages; ++page_index) {
        if (entries_emitted >= kMaxImageEntries) break;

        FPDF_PAGE const page = FPDF_LoadPage(doc, page_index);
        if (page == nullptr) break;  // a corrupt page -- stop here, same fail-visible posture as text mode

        int const object_count = FPDFPage_CountObjects(page);
        for (int object_index = 0; object_index < object_count; ++object_index) {
            if (entries_emitted >= kMaxImageEntries) break;

            FPDF_PAGEOBJECT const object = FPDFPage_GetObject(page, object_index);
            if (object == nullptr) continue;
            if (FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_IMAGE) continue;

            FPDF_IMAGEOBJ_METADATA metadata{};
            if (!FPDFImageObj_GetImageMetadata(object, page, &metadata)) continue;

            std::string const payload = make_image_entry_payload(
                static_cast<std::uint32_t>(page_index), metadata.width, metadata.height,
                metadata.bits_per_pixel);

            std::size_t const record_bytes = 16 + payload.size();
            if (bytes_written + record_bytes > ceiling) {
                FPDF_ClosePage(page);
                return 0;  // stop before writing -- never a partial record
            }

            if (!write_record(kKindImageEntry, static_cast<std::uint32_t>(entries_emitted), payload)) {
                FPDF_ClosePage(page);
                return 3;
            }
            bytes_written += record_bytes;
            ++entries_emitted;
        }

        FPDF_ClosePage(page);
    }
    return 0;
}

// Emits the page count via the SAME kKindMeta record run_text_mode already uses (reused, not
// re-derived), then one kKindMetaField record per non-empty standard document-info tag (sparse --
// a tag PDFium reports empty/absent gets no record at all, see this file's own top comment). The 8
// tags are cheap (one FPDF_GetMetaText call each), so no output-ceiling early-exit loop is needed the
// way run_text_mode/emit_toc_subtree need one for a document with many pages/bookmarks -- the ceiling
// check before each write is still applied defensively, in case a pathological tag value is huge.
[[nodiscard]] int run_meta_mode(FPDF_DOCUMENT doc, std::size_t ceiling) {
    std::size_t bytes_written = 0;

    int const total_pages = FPDF_GetPageCount(doc);
    if (!write_record(kKindMeta, static_cast<std::uint32_t>(total_pages < 0 ? 0 : total_pages), {})) {
        return 3;
    }
    bytes_written += 16;  // the META record's own header

    for (auto const& entry : kMetaTagTable) {
        std::string const value = meta_text_utf8(doc, entry.tag);
        if (value.empty()) continue;  // sparse -- PDFium reports this tag empty/absent, skip it

        std::size_t const record_bytes = 16 + value.size();
        if (bytes_written + record_bytes > ceiling) break;  // stop BEFORE writing -- never a partial record

        if (!write_record(kKindMetaField, entry.index, value)) return 3;
        bytes_written += record_bytes;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Raw binary framing on stdout -- Windows' default text-mode CRLF translation would corrupt
    // both the fixed-width headers and any payload byte that happens to be 0x0A.
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (argc != 3) {
        std::fprintf(stderr, "usage: agentengine_pdf_worker <text|toc|images|meta> <pdf-path>\n");
        return 1;
    }
    std::string_view const mode = argv[1];
    if (mode != "text" && mode != "toc" && mode != "images" && mode != "meta") {
        std::fprintf(
            stderr,
            "agentengine_pdf_worker: unknown mode '%s' (expected 'text', 'toc', 'images', or 'meta')\n",
            argv[1]);
        return 1;
    }

    std::size_t const ceiling = compute_output_ceiling();

    FPDF_InitLibrary();

    FPDF_DOCUMENT const doc = FPDF_LoadDocument(argv[2], nullptr);
    if (doc == nullptr) {
        FPDF_DestroyLibrary();
        std::fprintf(stderr, "agentengine_pdf_worker: FPDF_LoadDocument failed (code %d)\n",
                     static_cast<int>(FPDF_GetLastError()));
        return 2;
    }

    int rc = 3;
    if (mode == "text") {
        rc = run_text_mode(doc, ceiling);
    } else if (mode == "toc") {
        rc = run_toc_mode(doc, ceiling);
    } else if (mode == "images") {
        rc = run_images_mode(doc, ceiling);
    } else {
        rc = run_meta_mode(doc, ceiling);
    }

    FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();
    return rc;
}
