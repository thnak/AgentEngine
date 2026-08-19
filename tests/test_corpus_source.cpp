// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.4/§2.4b --
// `DiskCorpusSource`/`RecursiveChunker` (core/corpus_source.hpp). Proves, against real temp files on
// disk, a small scripted mock `Embedder`, `InMemoryWorktreeObjectStore`, and
// `rt::InMemoryAppendLogStore`:
//   (a) mounting a small folder produces the expected chunk count in the index, with matching
//       blob + citation-record storage;
//   (b) §3 claim 2's own disproof test -- mutate exactly 1 of N files, re-mount with the previous
//       file-hash map, assert the mock embedder's own texts-embedded counter rises by EXACTLY the
//       mutated file's own new chunk count, not the whole corpus's;
//   (c) two files with byte-identical content produce ONE shared chunk in the index (§2.4A's
//       content-addressed dedup working as intended, not rejected as an error);
//   (d) `RecursiveChunker` actually splits a long input into multiple chunks, with the configured
//       overlap genuinely present at each chunk boundary.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/corpus_scope.hpp"
#include "agentengine/core/corpus_source.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// A deterministic stand-in for a real embedding backend -- mirrors examples/08_memory.cpp's
// `MockSummarizerClient` shape (a scripted, side-effect-free conformer, no network). The vector for
// a given input text is derived cheaply from that text's own std::hash + length, so different texts
// get different-but-deterministic vectors without needing a real embedding model. Also counts, for
// test (b)'s own purposes, how many TEXTS have been embedded across every `embed_batch()` call --
// the metric §3 claim 2's disproof test compares against "the mutated file's own chunk count".
class MockEmbedder {
public:
    [[nodiscard]] ae::EmbedderCapabilities capabilities() const { return caps; }

    ae::task<ae::result<std::vector<std::vector<float>>>> embed_batch(std::vector<std::string> const& texts,
                                                                        ae::EffectContext&) {
        ++call_count;
        texts_embedded += texts.size();
        std::vector<std::vector<float>> out;
        out.reserve(texts.size());
        for (auto const& t : texts) {
            std::size_t const h = std::hash<std::string>{}(t);
            out.push_back({static_cast<float>(t.size()), static_cast<float>(h % 997),
                            static_cast<float>((h / 997) % 991), 1.0f});
        }
        co_return out;
    }

    ae::EmbedderCapabilities caps{4, 0};  // dimensions=4, max_batch_size=0 (uncapped)
    std::size_t call_count = 0;
    std::size_t texts_embedded = 0;
};
static_assert(ae::Embedder<MockEmbedder>);

void write_file(std::filesystem::path const& path, std::string const& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// Splits `text` on the literal separator `merge_atoms_into_chunks` (core/corpus_source.hpp) joins
// atoms with -- test-only, so (d) can inspect individual atoms at a chunk boundary without
// depending on that header's internal `corpus_source_detail` namespace.
std::vector<std::string> split_on_double_newline(std::string const& text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        auto const pos = text.find("\n\n", start);
        if (pos == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 2;
    }
    return parts;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;

    // --- (a)/(b)/(c): mount, re-mount after a targeted mutation, cross-file content dedup ----------
    {
        fs::path const root = fs::temp_directory_path() / "ae_test_corpus_source_abc";
        std::error_code rm_ec;
        fs::remove_all(root, rm_ec);
        fs::create_directories(root);

        std::string const file1_original = "The quick brown fox jumps over the lazy dog.";
        std::string const shared_content = "Duplicate boilerplate content shared verbatim by two files.";
        write_file(root / "file1.txt", file1_original);
        write_file(root / "file2.txt", shared_content);
        write_file(root / "file3.txt", shared_content);  // byte-identical to file2.txt

        ae::InMemoryWorktreeObjectStore object_store;
        ae::rt::InMemoryAppendLogStore ref_store;
        ae::BruteForceCosineIndex index;
        MockEmbedder embedder;

        ae::Principal const principal{"p-corpus-test", "tenant-corpus"};
        ae::Mount const mount = ae::rag_corpus_mount(ae::corpus_scope::per_principal, principal, "test-corpus");
        ae::cap::FsWrite const write_cap{mount.mount_id, "", std::nullopt, std::nullopt};
        ae::cap::FsRead const read_cap{mount.mount_id, "", std::nullopt};

        ae::DiskCorpusSource source{root};
        ae::EffectContext ctx{};
        ctx.principal = principal;

        std::unordered_map<std::string, std::string> const empty_prior_hashes;
        auto first = ae::test_support::run_task_sync<ae::result<ae::DiskCorpusMountResult>>(
            source.mount(object_store, ref_store, mount, write_cap, embedder, index, ctx, empty_prior_hashes));

        AE_CHECK(first.has_value(), "(a) initial mount of a 3-file folder succeeds");
        if (!first.has_value()) {
            std::cout << "test_corpus_source: FAIL (initial mount error: " << first.error().message << ")\n";
            return 1;
        }

        AE_CHECK(first->files_scanned == 3, "(a) all 3 files were walked");
        AE_CHECK(first->files_changed == 3, "(a) every file is new on a first mount -- all 3 counted changed");
        AE_CHECK(first->chunks_seen == 3,
                 "(a) each short file (well under the ~450-word default) produced exactly 1 chunk, "
                 "3 total");
        AE_CHECK(first->chunks_deduped == 1,
                 "(c) file3.txt's chunk is byte-identical to file2.txt's -- deduped as a normal skip, "
                 "not an error");
        AE_CHECK(first->chunks_embedded == 2,
                 "(c) only 2 GENUINELY new chunks were embedded/indexed this pass -- file1's unique "
                 "chunk, and ONE shared chunk for file2+file3's identical content");
        AE_CHECK(index.size() == 2,
                 "(a)/(c) the index holds exactly 2 entries -- 3 files produced only 2 DISTINCT "
                 "chunks by content, the third being a pure content-addressed duplicate");
        AE_CHECK(embedder.texts_embedded == 2, "(a) the mock embedder was asked to embed exactly 2 texts");

        // --- (a) matching blob + citation-record storage ------------------------------------------
        bool found_file1_record = false;
        for (auto const& [path, hash] : first->file_hashes) {
            (void)hash;
            if (path == "file1.txt") found_file1_record = true;
        }
        AE_CHECK(found_file1_record, "(a) file_hashes carries an entry for file1.txt, keyed by its "
                                      "POSIX-relative path");

        // Recompute file1's own chunk id the same way the mount did, to look its record up directly.
        auto file1_blob_digest = ae::compute_digest(
            std::as_bytes(std::span{file1_original.data(), file1_original.size()}));
        AE_CHECK(file1_blob_digest.has_value(), "(a) setup: file1's own content digest computes cleanly");
        if (file1_blob_digest.has_value()) {
            AE_CHECK(index.contains(*file1_blob_digest),
                     "(a) file1's chunk id (its own whole-file digest, since it's a single-chunk "
                     "file) is present in the index");
            auto blob = object_store.get_blob(*file1_blob_digest);
            AE_CHECK(blob.has_value() && std::string(reinterpret_cast<char const*>(blob->data()), blob->size()) ==
                                             file1_original,
                     "(a) the chunk TEXT itself is retrievable from the object store by that same id "
                     "(put_blob() and the precomputed chunk id agree)");
            auto record = ae::read_corpus_chunk_record(object_store, ref_store, mount, read_cap, *file1_blob_digest);
            AE_CHECK(record.has_value() && record->source_path == "file1.txt" && record->line_start == 1 &&
                         record->line_end == 1,
                     "(a) the citation record for file1's chunk names the right source_path and a "
                     "1-indexed line range covering its single line");
        }

        // --- (b) mutate exactly 1 of 3 files, re-mount, assert O(k) embedder texts, not O(n) --------
        std::string const file1_mutated = file1_original + " A genuinely new sentence, mutated.";
        write_file(root / "file1.txt", file1_mutated);

        std::size_t const texts_embedded_before_remount = embedder.texts_embedded;
        auto second = ae::test_support::run_task_sync<ae::result<ae::DiskCorpusMountResult>>(
            source.mount(object_store, ref_store, mount, write_cap, embedder, index, ctx, first->file_hashes));

        AE_CHECK(second.has_value(), "(b) re-mount after mutating 1 of 3 files succeeds");
        if (second.has_value()) {
            AE_CHECK(second->files_scanned == 3, "(b) the re-mount still walks all 3 files");
            AE_CHECK(second->files_changed == 1 && second->files_unchanged == 2,
                     "(b) exactly the 1 mutated file is detected as changed -- the other 2 are "
                     "skipped entirely via source_file_hash comparison against the prior mount's map");
            AE_CHECK(second->chunks_embedded == 1,
                     "(b) exactly 1 new chunk (the mutated file's own single chunk) was embedded and "
                     "indexed this pass");

            std::size_t const texts_embedded_this_remount =
                embedder.texts_embedded - texts_embedded_before_remount;
            AE_CHECK(texts_embedded_this_remount == 1,
                     "§3 claim 2's disproof: the mock embedder's texts-embedded counter rose by "
                     "EXACTLY 1 -- the mutated file's own chunk count -- not 3 (the whole corpus's "
                     "chunk count), proving re-mount cost is O(k) in changed files, not O(n)");
            AE_CHECK(index.size() == 3,
                     "(b) the index now holds 3 entries: the 2 original distinct chunks plus the "
                     "mutated file's new chunk");
        }

        fs::remove_all(root, rm_ec);
    }

    // --- (d) RecursiveChunker splits a long input into multiple chunks with real overlap -----------
    {
        // 20 one-word paragraphs, each its own atom (word_count == 1) -- a controlled input whose
        // atom boundaries are exactly the paragraph boundaries, so the sliding-window overlap
        // between consecutive chunks is directly, precisely observable.
        std::string long_text;
        for (int i = 0; i < 20; ++i) {
            if (i > 0) long_text += "\n\n";
            long_text += "word" + std::to_string(i);
        }

        ae::RecursiveChunker const chunker{/*chunk_size_words=*/5, /*overlap_fraction=*/0.4};
        auto chunks = chunker.chunk(long_text);

        AE_CHECK(chunks.size() > 1,
                 "(d) a 20-word input with a 5-word target chunk size splits into multiple chunks");

        // With chunk_size_words=5 and overlap_fraction=0.4, overlap_words = floor(5*0.4) = 2, and
        // every atom here is exactly 1 word -- so the merge's atom-granular walk-back (core/
        // corpus_source.hpp's `merge_atoms_into_chunks`) should back up EXACTLY 2 whole atoms at
        // every boundary: the last 2 atoms of chunk[i] are exactly the first 2 atoms of chunk[i+1],
        // not merely some single shared word.
        std::size_t const expected_overlap_atoms = 2;
        std::size_t boundaries_with_real_overlap = 0;
        for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
            auto const tail_atoms = split_on_double_newline(chunks[i].text);
            auto const head_atoms = split_on_double_newline(chunks[i + 1].text);
            AE_CHECK(tail_atoms.size() >= expected_overlap_atoms && head_atoms.size() >= expected_overlap_atoms,
                     "(d) both chunks at this boundary have at least as many atoms as the expected "
                     "overlap width");
            bool boundary_matches = false;
            if (tail_atoms.size() >= expected_overlap_atoms && head_atoms.size() >= expected_overlap_atoms) {
                boundary_matches = true;
                for (std::size_t k = 0; k < expected_overlap_atoms; ++k) {
                    std::string const& from_tail = tail_atoms[tail_atoms.size() - expected_overlap_atoms + k];
                    std::string const& from_head = head_atoms[k];
                    if (from_tail != from_head) boundary_matches = false;
                }
            }
            if (boundary_matches) ++boundaries_with_real_overlap;
        }
        AE_CHECK(boundaries_with_real_overlap == chunks.size() - 1,
                 "(d) EVERY chunk boundary has real, configured overlap: the trailing "
                 "expected_overlap_atoms atoms of chunk[i] are exactly the leading atoms of "
                 "chunk[i+1] -- not just adjacent, non-overlapping chunks");

        // Line ranges are 1-indexed and cover the whole input, monotonically non-decreasing.
        AE_CHECK(chunks.front().line_start == 1, "(d) the first chunk starts at line 1 (1-indexed)");
        bool monotonic = true;
        for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
            if (chunks[i + 1].line_start < chunks[i].line_start) monotonic = false;
        }
        AE_CHECK(monotonic, "(d) chunk line_start values are non-decreasing across the sequence");

        // A single short input (well under the target size) yields exactly ONE chunk, no spurious
        // splitting or overlap machinery kicking in for content that never needed it.
        ae::RecursiveChunker const default_chunker{};
        auto short_chunks = default_chunker.chunk("Just one short paragraph, nothing more.");
        AE_CHECK(short_chunks.size() == 1,
                 "(d) a short input (under the default ~450-word target) produces exactly 1 chunk");

        // An empty input produces zero chunks, not a spurious empty one.
        auto empty_chunks = default_chunker.chunk("");
        AE_CHECK(empty_chunks.empty(), "(d) an empty input produces zero chunks");
    }

    // --- (e) max_atom_bytes backstop: word-count alone does not bound byte size ---------------------
    // Red-team finding (2026-08-19): a line with very few "words" (whitespace-delimited) but one of
    // them a huge no-whitespace run (a minified blob/base64/URL) was previously kept as ONE unbounded
    // atom/chunk, since `recursive_split_atom`'s own check is `word_count <= max_words`, never a byte
    // count. This proves the fix: `enforce_max_atom_bytes` splits such a line by raw bytes regardless
    // of its word count, and every resulting chunk stays within the configured byte cap.
    {
        std::string huge_word(2000, 'x');  // a single 2000-byte run with NO internal whitespace at all
        std::string const pathological_line = "prefix " + huge_word + " suffix";  // word_count == 3

        // chunk_size_words deliberately generous (50) so word-count alone would NEVER trigger a
        // split -- only the byte-cap backstop can be responsible for any splitting observed here.
        ae::RecursiveChunker const capped_chunker{/*chunk_size_words=*/50, /*overlap_fraction=*/0.0,
                                                    /*max_atom_bytes=*/64};
        auto capped_chunks = capped_chunker.chunk(pathological_line);

        AE_CHECK(capped_chunks.size() > 1,
                 "(e) a word-count-tiny line dominated by one huge no-whitespace run is still split "
                 "into multiple chunks by the byte-cap backstop, not kept as one unbounded chunk");
        bool all_within_cap = true;
        for (auto const& c : capped_chunks) {
            if (c.text.size() > 64) all_within_cap = false;
        }
        AE_CHECK(all_within_cap,
                 "(e) every resulting chunk's byte length is within max_atom_bytes -- the backstop "
                 "actually bounds output size, not merely triggers without effect");

        std::size_t total_bytes = 0;
        for (auto const& c : capped_chunks) total_bytes += c.text.size();
        AE_CHECK(total_bytes == pathological_line.size(),
                 "(e) the hard byte split is lossless -- concatenating every piece's byte length "
                 "accounts for the whole original input, no content silently dropped");

        // max_atom_bytes == 0 explicitly disables the backstop (opt-out) -- the pathological line
        // then comes back as one unbounded atom/chunk again, same as before this fix, proving the
        // backstop is genuinely what changed behavior above, not some unrelated word-count effect.
        ae::RecursiveChunker const uncapped_chunker{/*chunk_size_words=*/50, /*overlap_fraction=*/0.0,
                                                      /*max_atom_bytes=*/0};
        auto uncapped_chunks = uncapped_chunker.chunk(pathological_line);
        AE_CHECK(uncapped_chunks.size() == 1 && uncapped_chunks.front().text.size() == pathological_line.size(),
                 "(e) max_atom_bytes=0 disables the backstop entirely -- the same pathological line "
                 "again produces one unsplit, unbounded chunk, confirming the default (16384) is what "
                 "protects real callers, not some other code path");
    }

    std::cout << (g_failures == 0 ? "test_corpus_source: OK\n" : "test_corpus_source: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
