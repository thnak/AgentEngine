#pragma once
// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.4, §4 findings 1/2
// — the citation-metadata record shared between a `CorpusSource` conformer (writes it at mount
// time) and `VectorRagContextProvider` (reads it at query time to render citations). Deliberately a
// THIRD storage artifact, distinct from `VectorIndex` (stores only `{id, vector}`) and
// `WorktreeObjectStore`'s blob (stores only the chunk TEXT) — this is the manifest/lookup §4
// finding 1/2 named as missing: "does this digest already have a stored vector" (via
// `source_file_hash` comparison at re-mount) and "where does a chunk's source_path/line_range live
// for citation rendering."

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine {

// ae-naming-lint: allow CorpusChunkRecord — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
struct CorpusChunkRecord {
    std::string id;  // content digest of the chunk TEXT -- the same id VectorIndex::add_batch()
                      // keys this chunk's vector by, and put_blob()'s own returned digest for it
    std::string source_path;  // relative to the mounted folder's root, POSIX-joined (mirrors
                               // DiskSkillSource::collect_files's own `lexically_relative`
                               // derivation, skill_source.hpp:99-126) -- cannot escape the mount
                               // root by construction
    std::size_t line_start = 0;
    std::size_t line_end = 0;
    std::string source_file_hash;  // the WHOLE source file's content hash at the mount/re-mount
                                    // that produced this chunk -- §3 claim 2's re-mount dedup
                                    // mechanism compares this against a prior mount's recorded
                                    // value to decide whether a file changed (unchanged -> skip
                                    // re-chunking/re-embedding that file entirely)

    friend bool operator==(CorpusChunkRecord const&, CorpusChunkRecord const&) = default;
};

[[nodiscard]] inline std::string corpus_chunk_record_path(std::string const& id) {
    return "chunks/" + id + ".json";
}

[[nodiscard]] inline json::Value corpus_chunk_record_to_json(CorpusChunkRecord const& r) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("id", json::Value::make_string(r.id));
    obj.emplace_back("source_path", json::Value::make_string(r.source_path));
    obj.emplace_back("line_start", json::Value::make_number(static_cast<double>(r.line_start)));
    obj.emplace_back("line_end", json::Value::make_number(static_cast<double>(r.line_end)));
    obj.emplace_back("source_file_hash", json::Value::make_string(r.source_file_hash));
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline result<CorpusChunkRecord> corpus_chunk_record_from_json(json::Value const& v) {
    auto require_string = [&](char const* key) -> result<std::string> {
        auto const* field = v.find(key);
        if (field == nullptr || !field->is_string()) {
            return std::unexpected(error{failure_class::contract,
                                          std::string("CorpusChunkRecord JSON missing string field: ") + key,
                                          "corpus_chunk.malformed_record"});
        }
        return field->as_string();
    };

    CorpusChunkRecord r{};
    auto id = require_string("id");
    if (!id) return std::unexpected(id.error());
    r.id = std::move(*id);

    auto source_path = require_string("source_path");
    if (!source_path) return std::unexpected(source_path.error());
    r.source_path = std::move(*source_path);

    // Reject, don't default to 0 (red-team, 2026-08-19 -- was previously silently coerced): a
    // missing/non-numeric line_start/line_end is a malformed record, not a legitimate "line 0"
    // citation. Inconsistent with this same function's own `id`/`source_path`/`source_file_hash`
    // handling to treat it any more leniently.
    auto require_number = [&](char const* key) -> result<std::size_t> {
        auto const* field = v.find(key);
        if (field == nullptr || !field->is_number()) {
            return std::unexpected(error{failure_class::contract,
                                          std::string("CorpusChunkRecord JSON missing numeric field: ") + key,
                                          "corpus_chunk.malformed_record"});
        }
        return static_cast<std::size_t>(field->as_number());
    };

    auto line_start = require_number("line_start");
    if (!line_start) return std::unexpected(line_start.error());
    r.line_start = *line_start;

    auto line_end = require_number("line_end");
    if (!line_end) return std::unexpected(line_end.error());
    r.line_end = *line_end;

    auto hash = require_string("source_file_hash");
    if (!hash) return std::unexpected(hash.error());
    r.source_file_hash = std::move(*hash);

    return r;
}

// Writes a chunk record at a deterministic path ("chunks/<id>.json") under `mount` — mirrors
// `write_memory_item()`'s own JSON-blob-via-`mount_write()` shape (memory.hpp:279-303) exactly,
// minus the write_seq stamping Memory needs and this record doesn't.
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<Ref> write_corpus_chunk_record(OS& object_store, RS& ref_store, Mount const& mount,
                                                     cap::FsWrite const& granted,
                                                     CorpusChunkRecord const& record) {
    std::string const text = json::dump(corpus_chunk_record_to_json(record));
    auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return mount_write(object_store, ref_store, mount, granted, corpus_chunk_record_path(record.id), bytes);
}

// O(1) point lookup by id (never a tree walk) — the shape `VectorRagContextProvider`'s query-time
// citation rendering needs (looking up a handful of ids `VectorIndex::search()` returned, not the
// whole corpus). Deliberately does NOT mirror `list_memory_items()`'s "list everything" shape — a
// per-query full-corpus walk would be far more expensive than a citation lookup needs to be.
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<CorpusChunkRecord> read_corpus_chunk_record(OS& object_store, RS& ref_store,
                                                                  Mount const& mount,
                                                                  cap::FsRead const& granted,
                                                                  std::string const& id) {
    auto bytes = mount_read(object_store, ref_store, mount, granted, corpus_chunk_record_path(id));
    if (!bytes) return std::unexpected(bytes.error());
    std::string text(reinterpret_cast<char const*>(bytes->data()), bytes->size());
    auto parsed = json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    return corpus_chunk_record_from_json(*parsed);
}

}  // namespace agentengine
