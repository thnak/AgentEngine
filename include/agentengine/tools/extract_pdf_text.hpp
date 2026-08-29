#pragma once
// Implements 009-Plugin-and-Extension-System.md §7's "Document extraction" catalog row -- the PDF
// text-layer slice, per ADR-106 (PDFium, BSD-3-Clause, not poppler/mupdf) and
// docs/planning/pdf-text-extraction-design-draft.md (Revision 7, closed after six real red-team
// rounds). First real, host-reachable slice of that design; earlier work on this branch landed
// NativeJailBackend::grant_ro_path_once() alone (native_jail_backend.hpp), the small, PDFium-
// independent primitive this file's Windows half now actually uses.
//
// ISOLATION (S3a): PDFium never links into this process. The actual decode runs in a dedicated,
// per-call child process (agentengine_pdf_worker, pdf_worker_main.cpp), spawned via the SAME
// production `NativeJailBackend`/`LinuxNativeJailBackend` `create()`/`exec()` conformers every other
// sandboxed exec in this tree uses -- a PDFium memory-corruption bug now runs inside a throwaway,
// resource-capped child, never the address space holding every session's `CapabilitySet`/
// `BoundCapability` state.
//
// SOURCING: `url`/`path`, the same shape and dynamic-capability-check pattern `ReadContent` already
// established (`read_content.hpp`) -- this file reuses that file's free `parse_read_content_url`/
// `net_out_target_string` helpers rather than re-deriving URL parsing, but keeps its OWN fetch/
// capability-check logic (S2's "share `fetch_source_bytes` vs. stay standalone" question is
// deliberately left open by the design draft; this ships as the fully-standalone option (a), not
// worth `ReadContent`'s own error-code migration cost for a first real slice).
//
// SCRATCH FILE (S3a(ii)): the host still does the capability check and the read (inheriting
// `ReadContent`'s own already-shipped, already-accepted `FileSystemAdapter::read_file()` size gap --
// explicitly NOT a new defect this file introduces). The fetched bytes are written to a per-call
// file inside ONE fixed scratch directory. Windows: that directory is granted to the shared
// AppContainer profile exactly once via `grant_ro_path_once()` (real, measured dedup -- see that
// method's own test). Linux: `LinuxNativeJailBackend::exec()`'s mount namespace is already private
// and per-call/torn-down-with-the-child (round-6 red-team's own finding -- no AppContainer-shaped
// cross-session exposure exists there at all), so the scratch directory is an ordinary
// `SandboxSpec.mounts` read-only bind, no dedup mechanism needed.
//
// LINUX TOOLCHAIN MOUNTS -- a real, disclosed residual, not a design choice this file can avoid:
// `LinuxNativeJailBackend::exec()` hardcodes `execve("/bin/sh", {"/bin/sh", "-c", command, ...})`
// (linux_native_jail_backend.cpp) -- there is no way to invoke the worker binary without a working
// `/bin/sh` and its dynamic-linker dependencies reachable inside the jail. This file therefore
// mounts `/bin`/`/lib`/`/lib64`/`/usr` read-only, the same directories
// `tests/helpers/native_jail_linux_toolchain_mounts.hpp` already grants for its own (test-only)
// purposes -- that header's own comment names this precisely: "not a security-hardened guest_path
// scheme... production filesystem-visibility policy is 010's interpreter-level mediation, not this
// [...] scope." No such interpreter-level mediation exists for this native worker path today; this
// is real, broader-than-ideal jail visibility, disclosed here rather than silently narrowed away.
//
// OUTPUT PARSING: `pdf_worker_main.cpp`'s own small, fixed binary record framing (magic/kind/index/
// payload_len, 16-byte header) -- duplicated by hand in `extract_pdf_text_detail::parse_worker_
// output` below since the worker is a separate binary with no shared header the tool side could
// `#include`; the two must be kept in sync by hand if the framing ever changes.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "agentengine/core/builtin_skills.hpp"  // builtin_skills_detail::parse_builtin() reuse, below
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/skill_source.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/sandbox/net_egress_proxy.hpp"
#include "agentengine/tools/read_content.hpp"
#include "agentengine/trust/capability.hpp"

#if defined(_WIN32)
#include "backends/native_jail/native_jail_backend.hpp"
#else
#include <unistd.h>

#include "backends/native_jail/linux_native_jail_backend.hpp"
#endif

namespace agentengine::tools {

// ae-naming-lint: allow ExtractPdfTextArgs — new tool, matches every other Args type's own naming
struct ExtractPdfTextArgs {
    // Exactly one of the two must be set (mirrors ReadContentArgs's own contract).
    std::optional<std::string> url;
    std::optional<std::string> path;
};
AE_JSON_SCHEMA(ExtractPdfTextArgs, url, path)

// ae-naming-lint: allow ExtractPdfTextReply — new tool, matches every other Reply type's own naming
struct ExtractPdfTextReply {
    std::string preview;
    bool truncated = false;
    std::uint64_t total_bytes = 0;
    // Split per round-1 red-team finding 5 (Revision 1's single `page_count` field couldn't express
    // both facts): `total_page_count` is the document's real page count; `pages_processed` is how
    // many pages actually got extracted before any cap (S4) stopped the run.
    std::uint32_t total_page_count = 0;
    std::uint32_t pages_processed = 0;
    bool truncated_pages = false;
    std::optional<BlobRef> blob;
};
AE_JSON_SCHEMA(ExtractPdfTextReply, preview, truncated, total_bytes, total_page_count, pages_processed,
               truncated_pages, blob)

namespace extract_pdf_text_detail {

inline constexpr std::uint64_t kDefaultPreviewByteCap = 8000;  // mirrors read_content_detail's own

// Must match pdf_worker_main.cpp's own constants exactly (kMagic/kKindMeta/kKindPage) -- see this
// file's own top comment.
inline constexpr std::uint32_t kMagic = 0x31504541u;
inline constexpr std::uint32_t kKindMeta = 0;
inline constexpr std::uint32_t kKindPage = 1;

// ae-naming-lint: allow ParsedPages — internal detail type, not a schema-bearing Args/Reply
struct ParsedPages {
    std::uint32_t total_page_count = 0;
    std::vector<std::string> pages;  // contiguous 0..N-1 prefix -- see parse_worker_output() below
};

// Parses pdf_worker_main.cpp's binary record stream. Stops at the first structurally invalid or
// incomplete record (a truncated trailing record from a killed/self-limited worker, an unexpected
// magic/kind) rather than guessing -- `pages` therefore always reflects a real, contiguous,
// verified-complete prefix of what the worker actually produced, never a partially-read record.
[[nodiscard]] inline result<ParsedPages> parse_worker_output(std::string const& raw) {
    ParsedPages out;
    bool got_meta = false;
    std::size_t offset = 0;
    while (offset + 16 <= raw.size()) {
        std::uint32_t header[4];
        std::memcpy(header, raw.data() + offset, sizeof(header));
        if (header[0] != kMagic) break;
        std::uint32_t const kind = header[1];
        std::uint32_t const index = header[2];
        std::uint32_t const payload_len = header[3];
        if (offset + 16 + payload_len > raw.size()) break;  // incomplete trailing record -- discard

        if (kind == kKindMeta) {
            out.total_page_count = index;
            got_meta = true;
        } else if (kind == kKindPage) {
            if (static_cast<std::size_t>(index) != out.pages.size()) break;  // non-contiguous -- stop
            out.pages.push_back(raw.substr(offset + 16, payload_len));
        } else {
            break;  // unknown kind -- stop rather than guess
        }
        offset += 16 + payload_len;
    }
    if (!got_meta) {
        return std::unexpected(error{failure_class::transient,
                                      "the pdf worker produced no META record (crashed before "
                                      "reporting a page count, or its output was corrupted)",
                                      "extract_pdf_text.worker_output_malformed"});
    }
    return out;
}

// Source-agnostic threshold/blob-promotion -- a deliberate, small, standalone copy of
// read_content_detail::build_reply's own logic (see this file's own top comment on why this stays
// standalone rather than sharing ReadContent's helper).
[[nodiscard]] inline result<ExtractPdfTextReply> build_reply(ParsedPages&& parsed, EffectContext& ctx) {
    std::string joined;
    for (auto const& page_text : parsed.pages) {
        joined += page_text;
        joined += '\n';
    }

    ExtractPdfTextReply reply;
    reply.total_bytes = joined.size();
    reply.total_page_count = parsed.total_page_count;
    reply.pages_processed = static_cast<std::uint32_t>(parsed.pages.size());
    reply.truncated_pages = reply.pages_processed < reply.total_page_count;

    std::uint64_t const threshold = ctx.tool_result_byte_threshold.value_or(kDefaultPreviewByteCap);
    if (reply.total_bytes <= threshold) {
        reply.preview = std::move(joined);
        reply.truncated = false;
        return reply;
    }

    reply.preview = joined.substr(0, static_cast<std::size_t>(threshold));
    reply.truncated = true;
    if (ctx.blob_sink) {
        std::span<std::byte const> const bytes{reinterpret_cast<std::byte const*>(joined.data()),
                                                 joined.size()};
        auto blob = ctx.blob_sink(bytes, "text/plain");
        if (!blob) return std::unexpected(blob.error());
        reply.blob = *blob;
    }
    return reply;
}

// ---- Fetching bytes for the two sources -- own minimal copy of ReadContent's pattern ----

template <sandbox::NetEgressBackend Proxy>
[[nodiscard]] result<std::string> fetch_via_url(std::string const& url, EffectContext& ctx,
                                                 Proxy const& proxy) {
    auto parsed = parse_read_content_url(url);
    if (!parsed) return std::unexpected(parsed.error());
    if (!ctx.capabilities) {
        return std::unexpected(error{failure_class::policy, "no capability set bound to this call",
                                      "tool.capability_not_held"});
    }
    std::string const target = net_out_target_string(*parsed);
    auto granted = ctx.capabilities->find_net_out(target);
    if (!granted) {
        return std::unexpected(error{failure_class::policy,
                                      "no granted NetOut capability covers '" + target + "'",
                                      "tool.capability_not_held"});
    }
    sandbox::NetEgressRequest const req{"GET", parsed->path, {}, {}};
    auto response = proxy.fetch(req, *granted);
    if (!response) return std::unexpected(response.error());
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(error{failure_class::transient,
                                      "extract_pdf_text received HTTP status " +
                                          std::to_string(response->status),
                                      "extract_pdf_text.http_error"});
    }
    return std::move(response->body);
}

[[nodiscard]] inline result<std::string> fetch_via_path(std::string const& path, EffectContext& ctx) {
    if (!ctx.sandbox_fs) {
        return std::unexpected(error{failure_class::resource,
                                      "this session has no sandbox mount yet",
                                      "extract_pdf_text.no_sandbox"});
    }
    if (!ctx.capabilities) {
        return std::unexpected(error{failure_class::policy, "no capability set bound to this call",
                                      "tool.capability_not_held"});
    }
    std::string const mount = "work";
    auto granted = ctx.capabilities->find_fs_read(mount, path);
    if (!granted) {
        return std::unexpected(error{failure_class::policy,
                                      "no granted FsRead capability covers '" + path + "' on the '" +
                                          mount + "' mount",
                                      "tool.capability_not_held"});
    }
    auto bytes = ctx.sandbox_fs->read_file(path);
    if (!bytes) return std::unexpected(bytes.error());
    return std::string(reinterpret_cast<char const*>(bytes->data()), bytes->size());
}

// ---- Scratch file + sandboxed worker invocation ----

[[nodiscard]] inline std::filesystem::path scratch_dir() {
#if defined(_WIN32)
    return std::filesystem::temp_directory_path() / "agentengine_pdf_scratch";
#else
    return std::filesystem::path("/tmp/agentengine_pdf_scratch");
#endif
}

// pid + a process-local monotonic counter -- collision-safety across CONCURRENT calls within one
// process (the design draft's own "not decided" item); not a security boundary (the scratch
// directory's own access grant, not the filename, is what gates who can read it).
[[nodiscard]] inline std::string unique_scratch_filename() {
    static std::atomic<std::uint64_t> counter{0};
    std::uint64_t const n = counter.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    unsigned long const pid = GetCurrentProcessId();
#else
    pid_t const pid = ::getpid();
#endif
    return "pdf_" + std::to_string(static_cast<long long>(pid)) + "_" + std::to_string(n) + ".pdf";
}

inline constexpr std::uint64_t kWorkerWallMs = 15000;
inline constexpr std::uint64_t kWorkerMemoryBytes = 256ull * 1024 * 1024;
inline constexpr std::uint32_t kWorkerPids = 4;
inline constexpr std::uint64_t kWorkerOutputBytes = 1024 * 1024;

#if defined(_WIN32)

[[nodiscard]] inline result<std::string> invoke_worker(std::filesystem::path const& pdf_file,
                                                         EffectContext& ctx) {
    // Process-wide by construction: shared_profile()/the AppContainer SID underneath NativeJailBackend
    // are already deployment-scoped statics (native_jail_backend.cpp), so a second, tool-owned
    // instance here shares the exact same underlying OS state as any other NativeJailBackend in this
    // process -- no new isolation boundary, just a convenient C++ handle to reach it from a Tool<>
    // that has no other path to one (EffectContext carries no NativeJailBackend& today; wiring one in
    // is real, separately-scoped host-integration work per the design draft's own S3a "not decided").
    static native_jail::NativeJailBackend backend;

    auto granted = backend.grant_ro_path_once(scratch_dir().wstring());
    if (!granted) return std::unexpected(granted.error());
    // The worker binary's own directory (also holds pdfium.dll, copied there at build time) is NOT
    // covered by any Windows default AppContainer ACE the way a curated set of real OS paths is
    // (ADR-004 §6 finding 1) -- an ordinary build-output directory needs its own explicit grant, the
    // same reason create_python_worker() grants its own exe_dir separately from ordinary
    // SandboxSpec.mounts (native_jail_backend.cpp).
    std::filesystem::path const worker_dir =
        std::filesystem::path(AE_PDF_WORKER_EXE_PATH).parent_path();
    auto worker_dir_granted = backend.grant_ro_path_once(worker_dir.wstring());
    if (!worker_dir_granted) return std::unexpected(worker_dir_granted.error());

    SandboxSpec spec;
    spec.limits.wall_ms = kWorkerWallMs;
    spec.limits.memory_bytes = kWorkerMemoryBytes;
    spec.limits.pids = kWorkerPids;
    spec.limits.output_bytes = kWorkerOutputBytes;

    auto handle = backend.create(spec, ctx);
    if (!handle) return std::unexpected(handle.error());

    std::string const cmdline =
        "\"" + std::string(AE_PDF_WORKER_EXE_PATH) + "\" \"" + pdf_file.string() + "\"";
    ExecRequest const req{.language = "native", .source = cmdline};
    auto outcome = backend.exec(*handle, req, ctx);
    backend.destroy(*handle);
    if (!outcome) return std::unexpected(outcome.error());
    if (outcome->klass != exec_outcome_class::ok) {
        return std::unexpected(error{failure_class::transient,
                                      "the pdf worker did not complete normally (class=" +
                                          std::to_string(static_cast<int>(outcome->klass)) + ")",
                                      "extract_pdf_text.worker_failed"});
    }
    return std::move(outcome->stdout_text);
}

#else  // Linux

[[nodiscard]] inline result<std::string> invoke_worker(std::filesystem::path const& pdf_file,
                                                         EffectContext& ctx) {
    static native_jail::LinuxNativeJailBackend backend;

    SandboxSpec spec;
    // Toolchain mounts -- see this file's own top comment: LinuxNativeJailBackend::exec() hardcodes
    // `/bin/sh -c`, so a working shell + dynamic linker must be reachable inside the jail. Real,
    // disclosed residual, not a security-hardened guest_path scheme.
    for (char const* dir : {"/bin", "/lib", "/lib64", "/usr"}) {
        std::error_code ec;
        if (std::filesystem::exists(dir, ec) && !ec) {
            spec.mounts.push_back(
                MountSpec{.source = std::string(dir), .guest_path = dir, .read_write = false});
        }
    }
    std::filesystem::path const worker_dir =
        std::filesystem::path(AE_PDF_WORKER_EXE_PATH).parent_path();
    spec.mounts.push_back(MountSpec{
        .source = worker_dir.string(), .guest_path = worker_dir.string(), .read_write = false});
    std::filesystem::path const scratch = scratch_dir();
    spec.mounts.push_back(MountSpec{
        .source = scratch.string(), .guest_path = scratch.string(), .read_write = false});

    spec.limits.wall_ms = kWorkerWallMs;
    spec.limits.memory_bytes = kWorkerMemoryBytes;
    spec.limits.pids = kWorkerPids;
    spec.limits.output_bytes = kWorkerOutputBytes;

    auto handle = backend.create(spec, ctx);
    if (!handle) return std::unexpected(handle.error());

    std::string const cmdline =
        "\"" + std::string(AE_PDF_WORKER_EXE_PATH) + "\" \"" + pdf_file.string() + "\"";
    ExecRequest const req{.language = "native", .source = cmdline};
    auto outcome = backend.exec(*handle, req, ctx);
    backend.destroy(*handle);
    if (!outcome) return std::unexpected(outcome.error());
    if (outcome->klass != exec_outcome_class::ok) {
        return std::unexpected(error{failure_class::transient,
                                      "the pdf worker did not complete normally (class=" +
                                          std::to_string(static_cast<int>(outcome->klass)) + ")",
                                      "extract_pdf_text.worker_failed"});
    }
    return std::move(outcome->stdout_text);
}

#endif

}  // namespace extract_pdf_text_detail

// 009 §7's PDF-text-layer document-extraction candidate. Capability model mirrors `ReadContent`
// exactly: `Capabilities<>` empty static ceiling (the target isn't known until call time for either
// source), dynamic `find_net_out`/`find_fs_read` checks inside `invoke()`. No new capability kind --
// the source capability that already gates "can this call see these bytes at all" is the right (and
// only) gate; decoding them into text is not a distinct authority (I2/I3).
// ae-naming-lint: allow ExtractPdfText — 009 §7's document-extraction candidate tool; 027 not yet updated
struct ExtractPdfText : Tool<ExtractPdfText, Capabilities<>, Approval<approval_mode::never_require>,
                              EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "extract_pdf_text";
    static constexpr std::string_view description =
        "Extract text from a PDF under one of this run's granted network-egress hosts or sandbox "
        "paths. Set exactly one of 'url' or 'path'. Returns decoded text (a bounded preview, "
        "promoted to a blob above the run's size threshold), plus total_page_count/pages_processed/"
        "truncated_pages when the document was too large or complex to fully process in one call.";

    using Args = ExtractPdfTextArgs;
    using Reply = ExtractPdfTextReply;

    [[nodiscard]] static result<Reply> invoke(Args args, EffectContext& ctx) {
        return invoke_via(args, ctx, sandbox::HostEgressProxy{});
    }

    // Parameterized over the egress backend, same testability seam ReadContent::invoke_via already
    // establishes -- invoke() above always supplies the real HostEgressProxy; a test supplies a fake
    // conformer instead.
    template <sandbox::NetEgressBackend Proxy>
    [[nodiscard]] static result<Reply> invoke_via(Args const& args, EffectContext& ctx,
                                                    Proxy const& proxy) {
        bool const has_url = args.url.has_value();
        bool const has_path = args.path.has_value();
        if (has_url == has_path) {
            return std::unexpected(error{failure_class::contract,
                                          "extract_pdf_text requires exactly one of 'url' or 'path'",
                                          "extract_pdf_text.ambiguous_source"});
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
                                          "extract_pdf_text.scratch_dir_failed"});
        }
        std::filesystem::path const file = dir / extract_pdf_text_detail::unique_scratch_filename();
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            if (!out) {
                return std::unexpected(error{failure_class::resource,
                                              "failed to open the pdf scratch file for writing",
                                              "extract_pdf_text.scratch_write_failed"});
            }
            out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
            if (!out) {
                return std::unexpected(error{failure_class::resource,
                                              "failed to write the pdf scratch file",
                                              "extract_pdf_text.scratch_write_failed"});
            }
        }

        auto raw_output = extract_pdf_text_detail::invoke_worker(file, ctx);
        std::filesystem::remove(file, ec);  // best-effort cleanup regardless of outcome below

        if (!raw_output) return std::unexpected(raw_output.error());

        auto parsed = extract_pdf_text_detail::parse_worker_output(*raw_output);
        if (!parsed) return std::unexpected(parsed.error());

        return extract_pdf_text_detail::build_reply(std::move(*parsed), ctx);
    }
};

// The companion skill (design draft S5) -- follows §8f's real, tested `builtin_skills.hpp` pattern
// exactly, reusing that header's own `builtin_skills_detail::parse_builtin()` construction helper.
// Deliberately NOT folded into `make_builtin_skills_source()`'s unconditional "always 5" list --
// that header compiles and mounts its skills unconditionally, in every build, regardless of whether
// `extract_pdf_text` exists at all (`AGENTENGINE_WITH_PDF` gates this whole file). Teaching the
// model about a tool that isn't there in a non-PDF build would be a real, if quiet, correctness bug.
// A host that mounts `extract_pdf_text` calls `make_extracting_document_text_skill_source()`
// alongside it, the same way any other non-builtin skill source gets added to a `SkillsProvider`.
inline constexpr std::string_view kExtractingDocumentTextSkillMd = R"SKILL(---
name: extracting-document-text
description: When and how to use extract_pdf_text instead of reading a PDF's raw bytes -- it returns decoded text, not a file to parse yourself, and reports how far it got on a document too large or complex to fully process in one call, with no automatic continuation. Use this before calling extract_pdf_text.
allowed-tools: extract_pdf_text
metadata:
  version: "1"
---
# Extracting document text

`extract_pdf_text(url=... | path=...) -> preview, truncated, total_bytes, total_page_count,
pages_processed, truncated_pages, blob` extracts DECODED TEXT from a PDF -- it does not hand you the
PDF's raw bytes. Never fetch a PDF via `read_content`'s `url`/`path` source and try to parse it
yourself in the code interpreter; call `extract_pdf_text` instead.

## The byte-threshold preview is pageable

When the extracted text itself is large, `truncated`/`blob` behave exactly like every other oversized
tool result (see `reading-large-content`): a bounded preview, plus a real, openable reference to the
rest. That guidance applies here unchanged.

## `total_page_count`/`pages_processed`/`truncated_pages` are a DIFFERENT signal -- read this carefully

These three fields mean the DOCUMENT ITSELF was too large or complex to fully process in ONE call --
not that the text was long. `truncated_pages` is true when `pages_processed < total_page_count`.

Unlike the byte-threshold preview above, there is NO follow-up call that resumes where extraction
stopped -- no page offset, no continuation token, nothing to page through. If you see
`truncated_pages: true`, the right move is narrowing the request (a smaller or split source
document) or treating the returned pages as genuinely partial, not requesting "the rest" as if one
more call would fetch it -- that call does not exist.
)SKILL";

[[nodiscard]] inline SkillSourceDescriptor make_extracting_document_text_skill_source() {
    result<std::vector<SkillSourceResult>> resolved = [] {
        auto parsed = builtin_skills_detail::parse_builtin("extracting-document-text",
                                                             kExtractingDocumentTextSkillMd);
        if (!parsed) return result<std::vector<SkillSourceResult>>(std::unexpected(parsed.error()));
        std::vector<SkillSourceResult> skills;
        skills.push_back(std::move(*parsed));
        return result<std::vector<SkillSourceResult>>(std::move(skills));
    }();

    SkillSourceDescriptor d;
    d.origin_id = "extract-pdf-text";
    d.load_skills = [resolved]() { return resolved; };
    return d;
}

}  // namespace agentengine::tools
