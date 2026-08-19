#pragma once
// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.6b — generalizes
// the marker-neutralization technique memory_provider.hpp proved for gap-audit finding 17 ("a
// ModelInferred/tool-derived item's own CONTENT can contain marker bytes that impersonate a
// higher-trust provenance label once rendered into context") into a shared utility parameterized
// over which marker family is being protected, instead of writing a second, duplicate mechanism
// for RAG citations. `memory_provider.hpp`'s own `neutralize_forged_memory_labels()` becomes a
// one-line call to this (behavior-preserving, not a rewrite of a working mechanism).

#include <string>
#include <string_view>

namespace agentengine {

// Every marker family sharing this utility opens with the same 3-byte UTF-8 glyph (U+27E6,
// "⟦", bytes E2 9F A6) followed by a family-specific tag ("memory:", "rag:", ...) — see
// `memory_label_open()` (memory_provider.hpp) and this ADR's own `"\xE2\x9F\xA6rag:"` literal.
// Every occurrence of `marker_open` found inside `content` is broken by inserting a zero-width
// space (U+200B) right after that shared leading glyph — the glyph itself survives unbroken
// (harmless alone), but the family tag that makes the marker recognizable does not — so the only
// place the exact, unbroken marker can ever appear in assembled text is a label the CALLER
// structurally emits after this function returns, never inside untrusted content passed through
// it. `marker_open` is always a fixed literal a caller controls (e.g. `memory_label_open()`,
// `rag_citation_label_open()`), never untrusted input itself.
[[nodiscard]] inline std::string neutralize_forged_provenance_markers(std::string const& content,
                                                                       std::string_view marker_open) {
    constexpr std::size_t kGlyphBytes = 3;
    std::string out;
    out.reserve(content.size());
    std::size_t pos = 0;
    while (true) {
        auto const found = content.find(marker_open, pos);
        if (found == std::string::npos) {
            out.append(content, pos, std::string::npos);
            break;
        }
        out.append(content, pos, found - pos);
        out.append(marker_open.substr(0, kGlyphBytes));
        out += "\xE2\x80\x8B";  // U+200B zero-width space -- breaks the exact "...<tag>" match
        out.append(marker_open.substr(kGlyphBytes));
        pos = found + marker_open.size();
    }
    return out;
}

}  // namespace agentengine
