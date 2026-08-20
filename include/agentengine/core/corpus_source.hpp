#pragma once
// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.4 (folder-source-
// mount, copy-at-mount semantics) and §2.4b (chunking strategy), and closes §4 finding 2 as far as
// this file's own scope reasonably allows ("the design never names a manifest/lookup that lets
// re-mount answer 'does this digest already have a stored vector' before calling embed_batch()
// again, nor where a per-file source_file_hash from a PRIOR mount is read back from for the
// staleness comparison"). `corpus_chunk.hpp`'s own file-top comment explains WHY there is
// deliberately no "list everything under this mount" path this file could use to reconstruct that
// prior-hash map itself: a per-query full-corpus walk would be far more expensive than the citation
// lookup that header is actually for. So the answer here is the same shape §2.4A's own text already
// implies -- push the bookkeeping to the CALLER: `DiskCorpusSource::mount()` returns the
// {source_path -> whole-file hash} map it just computed (`DiskCorpusMountResult::file_hashes`), and
// a caller who wants a cheap re-mount later passes that same map back in as `previous_file_hashes`.
// This file does not persist that map anywhere itself -- doing so would need a NEW durable artifact
// this ADR never names, and the caller already has the map in hand for free as this call's own
// return value.
//
// Folder-walk pattern mirrors `skill_source_detail::collect_files`/`DiskSkillSource`
// (core/skill_source.hpp) closely: `std::filesystem::recursive_directory_iterator`,
// `entry.path().lexically_relative(root)`, POSIX-joined relative paths that cannot escape the mount
// root, and the same all-or-nothing "rejects rather than guessing" stance on a hard filesystem
// error. Chunk identity mirrors `write_memory_item()`'s "identity, not assigned" pattern
// (core/memory.hpp): `compute_digest()` (core/worktree.hpp) is called directly, BEFORE `put_blob()`,
// because the resulting digest is needed for THREE things before storage ever happens --
// `VectorIndex::contains()`/`add_batch()`'s key, and `CorpusChunkRecord::id` -- not only for the
// blob's own address.
//
// Partial-failure policy (closes §4 finding 8, "embed_batch() partial-failure semantics are
// undefined"): **one real, stated behavior, matching DiskSkillSource's own all-or-nothing stance**
// (skill_source.hpp: "all-or-nothing... a caller must never silently run with half a source it
// believed loaded cleanly") -- a failing `embed_batch()` sub-batch aborts the WHOLE mount pass and
// returns that error. This is enforced structurally, not just by convention: every `embed_batch()`
// sub-batch for this pass's new chunks runs FIRST, entirely, before any of them is written to the
// index/store/chunk-record manifest -- so an error partway through embedding never leaves a chunk
// half-committed (present in the index but missing its record, or vice versa). Within the commit
// phase itself, `put_blob()`/`write_corpus_chunk_record()` run BEFORE `index.add_batch()` for each
// sub-batch (red-team, 2026-08-19, fixed the same pass -- see the commit-phase comment below for
// why the order matters): a failure partway through blob/record writes leaves those ids still
// `index.contains()`-false, so the NEXT mount retries them (idempotent), rather than leaving a
// permanent ghost entry in the index with no backing blob/record. What this does NOT fully close,
// named honestly: if a LATER sub-batch's writes fail after an EARLIER sub-batch's writes already
// succeeded, this mount pass still returns an error, but the earlier sub-batch's writes are not
// rolled back -- there is no transaction spanning multiple worktree commits in this codebase (the
// same "no compare-and-set on commit_ref()" limitation §4 finding 5 already names for concurrent
// writers applies here to partial-failure-within-one-writer too, a narrower instance of the
// identical missing primitive).
//
// Sub-batching (closes §4 finding 9, "no sub-batching story when ingestion exceeds
// EmbedderCapabilities.max_batch_size"): every NEW chunk across the whole mount pass is embedded
// through one logical `embed_batch()` call site, sub-batched into groups of at most
// `capabilities().max_batch_size` texts each (uncapped -- one sub-batch -- if `max_batch_size == 0`,
// i.e. a conformer that declares no limit).
//
// Named residuals this file deliberately does NOT address (recorded honestly, not silently
// omitted, matching this ADR's own §4/§7 style):
//   - **Stale-chunk garbage collection is not implemented.** When a file changes, its OLD chunk ids
//     (from the PRIOR mount, now no longer produced by this file's current content) are never
//     removed from the `VectorIndex` or from `chunks/<id>.json` in the object store -- they become
//     orphaned, unreachable-by-any-current-file entries that nonetheless remain fully live in the
//     index (a stale chunk can still be retrieved and cited at query time, even though the source
//     file that produced it no longer contains that text). Removing them needs either a real
//     `VectorIndex::remove()` operation (not in the `VectorIndex` concept today, core/vector_index.hpp)
//     or a mark-and-sweep pass keyed off `CorpusChunkRecord::source_path` -- neither exists, and this
//     file does not attempt either.
//   - **Vector-index persistence** (§4 finding 1) is entirely a `VectorIndex`-conformer-level
//     concern this file has no say over: whatever `IndexT` the caller supplies determines whether
//     `chunks_embedded` survives a process restart. `BruteForceCosineIndex` (the only conformer that
//     exists today) is in-memory-only, so within one process lifetime `previous_file_hashes` makes a
//     re-mount cheap, but nothing here changes that a fresh process starts with an empty index
//     regardless of what `previous_file_hashes` claims.
//   - **The concurrent-writer race** (§4 finding 5, `commit_ref()` has no compare-and-set) is
//     unaddressed here exactly as it is everywhere else in this codebase that writes through
//     `mount_write()` -- two concurrent `mount()` calls against the SAME corpus mount can silently
//     lose one writer's chunks. Not solved, only inherited.
//   - **Symlink/traversal policy** (§4 finding 13) is inherited from
//     `skill_source_detail::collect_files` unexamined, same as that finding already names for
//     `DiskSkillSource` itself -- `recursive_directory_iterator` here has no explicit symlink
//     handling of its own, and a folder corpus is plausibly pointed at a less-curated location than
//     a skill bundle directory.
//   - **Unmount lifecycle** (§4 finding 10) is entirely out of scope for this file -- there is no
//     "un-mount a corpus" operation here, matching the ADR's own residual list.
//   - **Citation-marker rendering/neutralization** (§2.6b) is a QUERY-time concern
//     (`VectorRagContextProvider::on_context()`, not built by this file) -- this file only writes
//     `CorpusChunkRecord`s, it never renders chunk text into a labeled citation string.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agentengine/core/corpus_chunk.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/embedder.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/vector_index.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// ================================================================================================
// §2.4b: the chunking policy seam -- "must be swappable, not hardcoded... CorpusSource/chunking
// should therefore take the chunking strategy as a declared, replaceable policy, matching this
// project's own CRTP-policy idiom elsewhere." A small concept (not a base class), matching
// `Embedder`/`VectorIndex`'s own shape one file over.
// ================================================================================================

// `line_start`/`line_end` are 1-indexed, inclusive, into the ORIGINAL source file -- what
// `CorpusChunkRecord::line_start`/`line_end` need for citation rendering (§2.6b), not an offset into
// this chunk's own (possibly re-joined, whitespace-normalized) `text`.
// ae-naming-lint: allow TextChunk — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
struct TextChunk {
    std::string text;
    std::size_t line_start = 0;
    std::size_t line_end   = 0;
};

template <class T>
// ae-naming-lint: allow ChunkingPolicy — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
concept ChunkingPolicy = requires(T const& c, std::string const& text) {
    { c.chunk(text) } -> std::same_as<std::vector<TextChunk>>;
};

namespace corpus_source_detail {

// ------------------------------------------------------------------------------------------------
// Honest approximation, stated plainly rather than pretended away: this codebase has no real
// tokenizer (no BPE/SentencePiece vocabulary, nothing provider-specific). A "token" below is
// approximated as one whitespace-delimited word -- cruder than a real subword tokenizer (real
// tokenizers typically produce MORE tokens than words for the same text), but a stated, consistent
// unit is what §2.4b's "roughly 400-512 tokens per chunk" needs to be actionable at all without
// pulling in a real tokenizer dependency this project doesn't have. `RecursiveChunker`'s own
// constructor comment repeats this so a reader who only sees that class still gets the caveat.
// ------------------------------------------------------------------------------------------------

[[nodiscard]] inline std::size_t count_words(std::string_view text) {
    std::size_t n = 0;
    bool in_word = false;
    for (char c : text) {
        bool const is_space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!is_space && !in_word) {
            ++n;
            in_word = true;
        } else if (is_space) {
            in_word = false;
        }
    }
    return n;
}

// Splits on '\n' -- one more line than the number of '\n' bytes, so a file ending with a trailing
// newline yields one extra, empty final "line" (matching how `std::getline`-shaped readers count
// lines; that trailing empty line is filtered out as blank content by `paragraphs_from_lines`
// below, so it never becomes a spurious empty paragraph). A trailing '\r' on any line is stripped
// (CRLF tolerance) so chunk text never carries a stray '\r' byte.
[[nodiscard]] inline std::vector<std::string> split_lines(std::string const& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            std::string line = text.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            start = i + 1;
        }
    }
    std::string last = text.substr(start);
    if (!last.empty() && last.back() == '\r') last.pop_back();
    lines.push_back(std::move(last));
    return lines;
}

[[nodiscard]] inline bool is_blank_line(std::string const& line) {
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) return false;
    }
    return true;
}

// One unit of text at some stage of the recursive split -- a whole paragraph before any splitting
// was needed, or a line/sentence/hard-split fragment once a bigger unit proved too large.
// `line_start`/`line_end` are always 1-indexed absolute line numbers into the ORIGINAL file,
// preserved through every level of the recursion (this is the whole reason the recursion tracks
// line numbers explicitly rather than working over one flat, re-joined string).
struct Atom {
    std::string text;
    std::size_t line_start = 0;
    std::size_t line_end   = 0;
    std::size_t word_count = 0;
};

// Groups `lines` into paragraph-level atoms: a maximal run of non-blank lines, separated by one or
// more blank lines (which are skipped entirely, contributing to no atom). This is the FIRST level
// of §2.4b's "paragraph -> line -> sentence fallback" recursive split -- a paragraph is the largest
// structural unit this chunker recognizes, and stays intact as one atom whenever it already fits
// under the target chunk size.
[[nodiscard]] inline std::vector<Atom> paragraphs_from_lines(std::vector<std::string> const& lines) {
    std::vector<Atom> paragraphs;
    std::size_t i = 0;  // 0-indexed into `lines`
    while (i < lines.size()) {
        if (is_blank_line(lines[i])) {
            ++i;
            continue;
        }
        std::size_t const start_0based = i;
        std::string joined = lines[i];
        std::size_t j = i + 1;
        while (j < lines.size() && !is_blank_line(lines[j])) {
            joined += "\n";
            joined += lines[j];
            ++j;
        }
        // 0-indexed [start_0based, j) -> 1-indexed inclusive [start_0based + 1, j].
        paragraphs.push_back(Atom{joined, start_0based + 1, j, count_words(joined)});
        i = j;
    }
    return paragraphs;
}

// Splits `text` into sentence-shaped fragments on '.'/'!'/'?' followed by whitespace or end-of-
// string -- a punctuation heuristic, not real sentence-boundary detection (no abbreviation table,
// no locale awareness). Good enough as the SECOND fallback level (after line-splitting hasn't
// helped, i.e. this is being called on a single already-too-long line): the goal at this level is
// "smaller than a whole line," not linguistic correctness.
[[nodiscard]] inline std::vector<std::string> split_sentences(std::string const& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    auto trim_leading = [](std::string s) {
        std::size_t const first = s.find_first_not_of(" \t");
        if (first == std::string::npos) return std::string{};
        return s.substr(first);
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        char const c = text[i];
        if (c != '.' && c != '!' && c != '?') continue;
        bool const boundary = (i + 1 == text.size()) || std::isspace(static_cast<unsigned char>(text[i + 1])) != 0;
        if (!boundary) continue;
        std::string sentence = trim_leading(text.substr(start, i - start + 1));
        if (!sentence.empty()) out.push_back(std::move(sentence));
        start = i + 1;
    }
    if (start < text.size()) {
        std::string rest = trim_leading(text.substr(start));
        if (!rest.empty()) out.push_back(std::move(rest));
    }
    return out;
}

// THIRD, last-resort fallback: `atom` has no sentence boundary at all (or is still too large even
// as one "sentence") -- split it into fixed-size word groups. `atom.line_start == atom.line_end` is
// preserved on every resulting fragment (this is only ever called on a single-line-derived atom, so
// there is only ever one line number to preserve).
inline void hard_split_by_words(Atom const& atom, std::size_t max_words, std::vector<Atom>& out) {
    std::vector<std::string> words;
    std::size_t word_start = 0;
    bool in_word = false;
    for (std::size_t i = 0; i <= atom.text.size(); ++i) {
        bool const is_space = (i == atom.text.size()) || std::isspace(static_cast<unsigned char>(atom.text[i])) != 0;
        if (!is_space && !in_word) {
            word_start = i;
            in_word = true;
        } else if (is_space && in_word) {
            words.push_back(atom.text.substr(word_start, i - word_start));
            in_word = false;
        }
    }
    if (words.empty()) {
        // Degenerate (should not happen: `word_count > 0` is checked by every caller before this
        // is reached) -- fall back to returning the atom unsplit rather than dropping content.
        out.push_back(atom);
        return;
    }
    for (std::size_t i = 0; i < words.size(); i += max_words) {
        std::size_t const end = std::min(i + max_words, words.size());
        std::string joined;
        for (std::size_t k = i; k < end; ++k) {
            if (k > i) joined += ' ';
            joined += words[k];
        }
        out.push_back(Atom{std::move(joined), atom.line_start, atom.line_end, end - i});
    }
}

// The recursive step itself: paragraph -> line -> sentence -> hard word split, stopping as soon as
// an atom already fits under `max_words`. Appends every resulting (small enough) atom to `out`.
inline void recursive_split_atom(Atom const& atom, std::size_t max_words, std::vector<Atom>& out) {
    if (atom.word_count == 0) return;  // a run of whitespace-only lines never reaches here in
                                        // practice (paragraphs_from_lines skips blank lines), but
                                        // guard against contributing a zero-content atom regardless.
    if (atom.word_count <= max_words) {
        out.push_back(atom);
        return;
    }
    if (atom.line_end > atom.line_start) {
        // Multi-line atom (a paragraph too big to keep whole): fall back to line-level atoms.
        auto lines = split_lines(atom.text);
        for (std::size_t k = 0; k < lines.size(); ++k) {
            std::size_t const abs_line = atom.line_start + k;
            Atom line_atom{lines[k], abs_line, abs_line, count_words(lines[k])};
            recursive_split_atom(line_atom, max_words, out);
        }
        return;
    }
    // Single line, still too big: sentence fallback, then (if that doesn't help either) a hard
    // word-count split.
    auto sentences = split_sentences(atom.text);
    if (sentences.size() <= 1) {
        hard_split_by_words(atom, max_words, out);
        return;
    }
    for (auto& s : sentences) {
        Atom sentence_atom{s, atom.line_start, atom.line_start, count_words(s)};
        recursive_split_atom(sentence_atom, max_words, out);  // one more fallback level if even a
                                                                // single "sentence" is still too big
    }
}

// Greedily packs already-small-enough `atoms` into chunks of at most `chunk_size_words` words each,
// with `overlap_words` worth of trailing atoms from each closed chunk repeated at the start of the
// next one (§2.4b's "10-20% overlap"). Overlap is atom-granular, not word-granular -- the boundary
// backs up by whole atoms until at least `overlap_words` words are covered (or until it would back
// up past the start of the chunk just closed, in which case overlap is skipped for that one
// boundary rather than looping forever): this keeps every chunk's text a clean concatenation of
// whole paragraph/line/sentence fragments, never a fragment cut mid-atom.
[[nodiscard]] inline std::vector<TextChunk> merge_atoms_into_chunks(std::vector<Atom> const& atoms,
                                                                     std::size_t chunk_size_words,
                                                                     std::size_t overlap_words) {
    std::vector<TextChunk> chunks;
    std::size_t i = 0;
    while (i < atoms.size()) {
        std::size_t words_acc = 0;
        std::size_t j = i;
        while (j < atoms.size()) {
            std::size_t const next_words = words_acc + atoms[j].word_count;
            if (words_acc > 0 && next_words > chunk_size_words) break;
            words_acc = next_words;
            ++j;
        }
        if (j == i) j = i + 1;  // defensive: guarantee forward progress even if a single atom alone
                                 // exceeds chunk_size_words (recursive_split_atom should prevent
                                 // this given max_words == chunk_size_words, but this loop must
                                 // never spin regardless).

        std::string text;
        for (std::size_t k = i; k < j; ++k) {
            if (k > i) text += "\n\n";
            text += atoms[k].text;
        }
        chunks.push_back(TextChunk{std::move(text), atoms[i].line_start, atoms[j - 1].line_end});

        if (j >= atoms.size()) break;

        std::size_t back_words = 0;
        std::size_t i_next = j;
        while (i_next > i && back_words < overlap_words) {
            --i_next;
            back_words += atoms[i_next].word_count;
        }
        if (i_next <= i) i_next = j;  // overlap would swallow the entire chunk just closed -- skip
                                       // overlap at this one boundary rather than looping forever.
        i = i_next;
    }
    return chunks;
}

// Additional safety net, independent of (and running AFTER) the word-count-based recursive split
// above -- a red-team pass (2026-08-19) found that word-count alone does not bound BYTE size: an
// atom that "fits" by word count can still be arbitrarily large if it is dominated by a run with no
// internal whitespace at all (a minified line, a base64 blob, a long hash/URL list).
// `recursive_split_atom`'s own `word_count <= max_words` check keeps such an atom whole regardless
// of its byte length -- e.g. a line with exactly 2 "words," one of them a 10 MB no-whitespace blob,
// has `word_count == 2` and is never subdivided, becoming one unbounded chunk sent whole to
// `Embedder::embed_batch()` (a real network call), `put_blob()`'d whole, and cited whole. Even
// `hard_split_by_words`'s own last-resort fallback does not help here, because that fallback is only
// ever REACHED when `word_count > max_words` -- a low-word-count-but-huge-bytes atom never triggers
// it. This function is the actual backstop: any atom whose byte length exceeds `max_atom_bytes` is
// hard-split into fixed-size byte pieces here, with each resulting piece's `word_count` forced to
// `max_words + 1` -- a deliberate SENTINEL, not a real word count -- so `merge_atoms_into_chunks`
// (which packs purely by summing `word_count`) can never merge two of these already-maximally-sized
// pieces into one oversized output chunk; each piece's sentinel value alone exceeds the packing
// budget, so each stands alone as its own chunk (see `merge_atoms_into_chunks`'s own first-atom-
// always-included rule). `max_atom_bytes == 0` disables this backstop entirely (opt-out, not a
// realistic default).
[[nodiscard]] inline std::vector<Atom> enforce_max_atom_bytes(std::vector<Atom> const& atoms,
                                                                std::size_t max_words,
                                                                std::size_t max_atom_bytes) {
    if (max_atom_bytes == 0) return atoms;
    std::vector<Atom> out;
    out.reserve(atoms.size());
    for (auto const& atom : atoms) {
        if (atom.text.size() <= max_atom_bytes) {
            out.push_back(atom);
            continue;
        }
        for (std::size_t start = 0; start < atom.text.size(); start += max_atom_bytes) {
            std::size_t const len = std::min(max_atom_bytes, atom.text.size() - start);
            out.push_back(Atom{atom.text.substr(start, len), atom.line_start, atom.line_end, max_words + 1});
        }
    }
    return out;
}

}  // namespace corpus_source_detail

// §2.4b's default conformer: paragraph -> line -> sentence -> hard-word-split recursive splitting,
// target ~450 "tokens" (whitespace-delimited words -- see the honest-approximation note in
// `corpus_source_detail` above, this is NOT a real tokenizer) per chunk with ~15% overlap, PLUS a
// hard byte-length backstop (`max_atom_bytes`, default 16 KiB) closing the word-count-only gap a
// red-team pass found (see `corpus_source_detail::enforce_max_atom_bytes`'s own comment). All three
// constants are overridable via the constructor per §2.4b's own "must be swappable" requirement (a
// caller who wants a DIFFERENT chunking algorithm entirely writes a different `ChunkingPolicy`
// conformer; a caller who just wants different constants for this same algorithm uses these
// constructor parameters).
// ae-naming-lint: allow RecursiveChunker — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
class RecursiveChunker {
public:
    explicit RecursiveChunker(std::size_t chunk_size_words = 450, double overlap_fraction = 0.15,
                               std::size_t max_atom_bytes = 16384)
        : chunk_size_words_(chunk_size_words > 0 ? chunk_size_words : 1),
          overlap_words_(static_cast<std::size_t>(static_cast<double>(chunk_size_words_) * overlap_fraction)),
          max_atom_bytes_(max_atom_bytes) {}

    [[nodiscard]] std::vector<TextChunk> chunk(std::string const& text) const {
        auto lines = corpus_source_detail::split_lines(text);
        auto paragraphs = corpus_source_detail::paragraphs_from_lines(lines);
        std::vector<corpus_source_detail::Atom> atoms;
        for (auto const& p : paragraphs) {
            corpus_source_detail::recursive_split_atom(p, chunk_size_words_, atoms);
        }
        atoms = corpus_source_detail::enforce_max_atom_bytes(atoms, chunk_size_words_, max_atom_bytes_);
        if (atoms.empty()) return {};
        return corpus_source_detail::merge_atoms_into_chunks(atoms, chunk_size_words_, overlap_words_);
    }

private:
    std::size_t chunk_size_words_;
    std::size_t overlap_words_;
    std::size_t max_atom_bytes_;
};
static_assert(ChunkingPolicy<RecursiveChunker>);

// ================================================================================================
// §2.4A: copy-at-mount folder ingestion with re-mount dedup (closes §4 finding 2's own "asserted,
// not designed" gap for THIS file's call site -- see the file-top comment for the full mechanism).
// ================================================================================================

// Returned by `DiskCorpusSource::mount()` -- `file_hashes` is the map a caller threads back in as
// the NEXT re-mount call's `previous_file_hashes` argument (see the file-top comment for why this
// file pushes that bookkeeping to the caller rather than persisting it itself). The rest are plain
// counters, useful for both real callers (progress/logging) and this file's own test (§3 claim 2's
// disproof needs to observe exactly how many chunks were newly embedded).
// ae-naming-lint: allow DiskCorpusMountResult — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
struct DiskCorpusMountResult {
    std::unordered_map<std::string, std::string> file_hashes;
    std::size_t files_scanned   = 0;
    std::size_t files_changed   = 0;  // new or content-changed since `previous_file_hashes`
    std::size_t files_unchanged = 0;  // skipped entirely: zero chunking, zero embedding, zero
                                       // `VectorIndex`/store/record writes for this file's chunks
    std::size_t chunks_seen     = 0;  // total chunks produced by chunking the CHANGED files
    std::size_t chunks_deduped  = 0;  // of those, how many were already present in the index
                                       // (content-addressed dedup, §2.4A -- a normal skip, not an
                                       // error, whether from an earlier mount or from a DIFFERENT
                                       // file earlier in this SAME pass)
    std::size_t chunks_embedded = 0;  // genuinely new chunks: embedded, added to the index, blobbed,
                                       // and recorded, this pass
};

// Folder-source-mount entry point (§2.4). Holds only the folder root -- mirrors `DiskSkillSource`'s
// own shape (`origin_id_`/`root_`) minus `origin_id` (this ADR names no equivalent concept for a
// corpus source; `mount.mount_id` already identifies which corpus this is).
// ae-naming-lint: allow DiskCorpusSource — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
class DiskCorpusSource {
public:
    explicit DiskCorpusSource(std::filesystem::path root) : root_(std::move(root)) {}

    // Walks `root_` once, chunks every file that is new or whose whole-file content hash differs
    // from `previous_file_hashes[relative_path]`, embeds and indexes every chunk from those files
    // that isn't already present in `index` (by content-addressed chunk id), and records citation
    // metadata for each newly-indexed chunk. All-or-nothing on a hard filesystem error or an
    // `embed_batch()` failure (see the file-top comment for the exact commit-ordering guarantee);
    // per-file/per-chunk skips (unchanged file, already-indexed chunk) are normal outcomes, not
    // errors, and are reflected only in the returned counters.
    template <Embedder EmbedderT, VectorIndex IndexT, WorktreeObjectStore OS, rt::AppendLogStore RS,
              ChunkingPolicy ChunkerT = RecursiveChunker>
    [[nodiscard]] task<result<DiskCorpusMountResult>> mount(
        OS& object_store, RS& ref_store, Mount const& mount, cap::FsWrite const& write_cap,
        EmbedderT& embedder, IndexT& index, EffectContext& ctx,
        std::unordered_map<std::string, std::string> const& previous_file_hashes,
        ChunkerT const& chunker = ChunkerT{}) const {
        std::error_code ec;
        if (!std::filesystem::is_directory(root_, ec) || ec) {
            co_return std::unexpected(error{failure_class::contract,
                                             "corpus source root is not a directory: " + root_.string(),
                                             "corpus_source.disk_root_not_a_directory"});
        }

        // Bootstrap the corpus worktree's ref if this is the very first mount -- `mount_write()`
        // (worktree.hpp) requires a ref that has already been committed at least once; this mirrors
        // `ensure_memory_worktree()`'s (core/memory.hpp) identical bootstrap for the same reason.
        // Idempotent: a re-mount against an already-bootstrapped ref takes the early `has_value()`
        // return and does nothing here.
        {
            auto existing = read_ref(ref_store, mount.ref_name);
            if (!existing) co_return std::unexpected(existing.error());
            if (!existing->has_value()) {
                auto empty = object_store.put_tree(Tree{});
                if (!empty) co_return std::unexpected(empty.error());
                auto committed = commit_ref(ref_store, mount.ref_name, *empty);
                if (!committed) co_return std::unexpected(committed.error());
            }
        }

        struct ChangedFile {
            std::string rel_path;
            std::string content;
            std::string file_hash;
        };
        std::vector<ChangedFile> changed_files;
        DiskCorpusMountResult out{};

        // --- Walk once, whole-file hash every entry, split into changed/unchanged --------------------
        // Mirrors `skill_source_detail::collect_files` (skill_source.hpp) exactly: same iterator,
        // same options, same `lexically_relative`+`generic_string()` POSIX-join, same "a walk error
        // aborts the whole call" stance.
        for (auto const& entry : std::filesystem::recursive_directory_iterator(
                 root_, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                co_return std::unexpected(error{failure_class::contract,
                                                 "failed to walk corpus source directory: " + ec.message(),
                                                 "corpus_source.disk_read_failed"});
            }
            if (!entry.is_regular_file()) continue;

            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) {
                co_return std::unexpected(error{failure_class::contract,
                                                 "failed to open corpus source file: " + entry.path().string(),
                                                 "corpus_source.disk_read_failed"});
            }
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string const posix_rel = entry.path().lexically_relative(root_).generic_string();
            ++out.files_scanned;

            auto content_bytes = std::as_bytes(std::span{content.data(), content.size()});
            auto file_hash = compute_digest(content_bytes);
            if (!file_hash) co_return std::unexpected(file_hash.error());

            auto const prev_it = previous_file_hashes.find(posix_rel);
            bool const unchanged = prev_it != previous_file_hashes.end() && prev_it->second == *file_hash;
            out.file_hashes.emplace(posix_rel, *file_hash);
            if (unchanged) {
                ++out.files_unchanged;
                continue;  // §2.4A: no chunking, no embedding, no index/store/record writes at all
                           // for this file's chunks -- this is the whole point of claim 2.
            }
            ++out.files_changed;
            changed_files.push_back(ChangedFile{posix_rel, std::move(content), *file_hash});
        }

        // --- Chunk every changed file, dedup against the index AND within this pass --------------------
        struct PendingChunk {
            std::string id;
            std::string text;
            std::string source_path;
            std::size_t line_start;
            std::size_t line_end;
            std::string source_file_hash;
        };
        std::vector<PendingChunk> pending;
        std::unordered_set<std::string> seen_ids_this_pass;

        for (auto const& f : changed_files) {
            auto chunks = chunker.chunk(f.content);
            out.chunks_seen += chunks.size();
            for (auto& tc : chunks) {
                auto chunk_bytes = std::as_bytes(std::span{tc.text.data(), tc.text.size()});
                auto id = compute_digest(chunk_bytes);
                if (!id) co_return std::unexpected(id.error());
                // §2.4A's own content-addressed dedup: two DIFFERENT files producing byte-identical
                // chunk text (real corpora case: shared license headers, boilerplate) is expected,
                // not an error -- `index.contains()` catches a chunk already indexed from a PRIOR
                // mount or an earlier file in THIS pass; `seen_ids_this_pass` additionally catches
                // two NEW chunks (from two different changed files) colliding within this one pass,
                // which `index.contains()` alone cannot see yet (neither has been added).
                if (index.contains(*id) || seen_ids_this_pass.contains(*id)) {
                    ++out.chunks_deduped;
                    continue;
                }
                seen_ids_this_pass.insert(*id);
                pending.push_back(
                    PendingChunk{*id, std::move(tc.text), f.rel_path, tc.line_start, tc.line_end, f.file_hash});
            }
        }

        if (pending.empty()) co_return out;

        // --- Embed ALL new chunks first (sub-batched), before committing ANYTHING -----------------------
        // §2.2B: batch is mandatory, not an optimization -- one logical embed_batch() call site for
        // the whole pass, sub-batched only to respect `EmbedderCapabilities.max_batch_size` (closes
        // §4 finding 9 for this call site). Every sub-batch must succeed before the commit phase
        // below runs at all -- see the file-top comment for why this ordering is what makes the
        // "abort the whole mount on embed failure" stance (closes §4 finding 8) actually hold rather
        // than merely being asserted.
        auto const caps = embedder.capabilities();
        std::size_t const batch_cap = caps.max_batch_size > 0 ? static_cast<std::size_t>(caps.max_batch_size)
                                                                : pending.size();
        std::vector<std::vector<float>> vectors(pending.size());
        for (std::size_t start = 0; start < pending.size(); start += batch_cap) {
            std::size_t const end = std::min(start + batch_cap, pending.size());
            std::vector<std::string> texts;
            texts.reserve(end - start);
            for (std::size_t k = start; k < end; ++k) texts.push_back(pending[k].text);

            auto embedded = co_await embedder.embed_batch(texts, ctx);
            if (!embedded) co_return std::unexpected(embedded.error());
            if (embedded->size() != texts.size()) {
                co_return std::unexpected(
                    error{failure_class::contract,
                          "embed_batch returned a different number of vectors than texts requested",
                          "corpus_source.embed_batch_size_mismatch"});
            }
            for (std::size_t k = 0; k < embedded->size(); ++k) vectors[start + k] = std::move((*embedded)[k]);
        }

        // --- Commit phase: blob + record FIRST, index.add_batch() LAST -----------------------------------
        // Ordering matters (red-team, 2026-08-19): `index.contains()` is the ONLY signal a future
        // re-mount uses to decide "already embedded, skip." Putting `add_batch()` first (as an
        // earlier version of this file did) meant a chunk could become permanently un-healable if a
        // LATER `put_blob()`/`write_corpus_chunk_record()` call in the same sub-batch failed after
        // it: the id would already be `index.contains()`-true, so every future re-mount would skip it
        // forever, even though it has no blob/record. Committing blob + record FIRST for every chunk
        // in the sub-batch, and calling `add_batch()` only after all of them succeed, means a failure
        // partway through blob/record writes leaves those ids still `index.contains()`-false -- the
        // NEXT mount will simply re-chunk, re-embed, and retry them (blob/record writes are
        // idempotent: content-addressed put_blob() and a deterministic record path both tolerate a
        // harmless re-write of identical content). This does not make the phase transactional (still
        // no rollback of an EARLIER sub-batch if a LATER one fails, per this file's own top comment),
        // it converts one specific failure mode from "permanent ghost entry" to "harmless retry."
        for (std::size_t start = 0; start < pending.size(); start += batch_cap) {
            std::size_t const end = std::min(start + batch_cap, pending.size());
            std::vector<std::string> ids;
            std::vector<std::vector<float>> batch_vecs;
            ids.reserve(end - start);
            batch_vecs.reserve(end - start);

            for (std::size_t k = start; k < end; ++k) {
                auto const& pc = pending[k];
                auto chunk_bytes = std::as_bytes(std::span{pc.text.data(), pc.text.size()});
                auto blob_digest = object_store.put_blob(chunk_bytes);
                if (!blob_digest) co_return std::unexpected(blob_digest.error());
                if (*blob_digest != pc.id) {
                    // Should be unreachable (both are `compute_digest()` over the identical bytes) --
                    // a real, checked assertion rather than a silently-trusted invariant, since
                    // `CorpusChunkRecord::id`'s own contract (corpus_chunk.hpp) depends on this
                    // equality holding.
                    co_return std::unexpected(error{failure_class::fatal,
                                                     "chunk blob digest does not match its precomputed id",
                                                     "corpus_source.chunk_id_mismatch"});
                }
                CorpusChunkRecord record{pc.id, pc.source_path, pc.line_start, pc.line_end, pc.source_file_hash};
                auto written = write_corpus_chunk_record(object_store, ref_store, mount, write_cap, record);
                if (!written) co_return std::unexpected(written.error());

                ids.push_back(pc.id);
                batch_vecs.push_back(vectors[k]);
            }

            auto added = index.add_batch(ids, batch_vecs);
            if (!added) co_return std::unexpected(added.error());
            out.chunks_embedded += (end - start);
        }

        co_return out;
    }

private:
    std::filesystem::path root_;
};

}  // namespace agentengine
