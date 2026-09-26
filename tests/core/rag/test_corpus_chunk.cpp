// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.4, §4 findings 1/2
// -- CorpusChunkRecord's storage round-trip (core/corpus_chunk.hpp), the shared contract between
// CorpusSource (writer, not yet implemented) and VectorRagContextProvider (reader, not yet
// implemented). Mirrors test_memory_worktree.cpp's own write/read-through-a-real-store shape.

#include <iostream>
#include <string>

#include "agentengine/core/corpus_chunk.hpp"
#include "agentengine/core/corpus_scope.hpp"
#include "agentengine/rt/append_log_store.hpp"

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

}  // namespace

int main() {
    ae::InMemoryWorktreeObjectStore object_store;
    ae::rt::InMemoryAppendLogStore ref_store;
    ae::Principal const principal{"p-1", "tenant-a"};
    ae::Mount const mount = ae::rag_corpus_mount(ae::corpus_scope::per_principal, principal, "internal-docs");

    // ensure_memory_worktree()'s own bootstrap shape, inlined here since corpus_scope.hpp
    // deliberately doesn't own worktree bootstrapping (that's CorpusSource's job, not yet built).
    {
        auto empty = object_store.put_tree(ae::Tree{});
        AE_CHECK(empty.has_value(), "setup: empty tree stored");
        auto committed = ae::commit_ref(ref_store, mount.ref_name, *empty);
        AE_CHECK(committed.has_value(), "setup: corpus worktree bootstraps");
    }

    ae::cap::FsWrite const write_cap{mount.mount_id, "", std::nullopt, std::nullopt};
    ae::cap::FsRead const read_cap{mount.mount_id, "", std::nullopt};

    ae::CorpusChunkRecord record{};
    record.id = "deadbeef";
    record.source_path = "guides/setup.md";
    record.line_start = 10;
    record.line_end = 42;
    record.source_file_hash = "sha256:abc123";

    auto written = ae::write_corpus_chunk_record(object_store, ref_store, mount, write_cap, record);
    AE_CHECK(written.has_value(), "write_corpus_chunk_record() succeeds through a real capability-gated mount");

    auto read_back = ae::read_corpus_chunk_record(object_store, ref_store, mount, read_cap, record.id);
    AE_CHECK(read_back.has_value() && *read_back == record,
             "read_corpus_chunk_record() round-trips every field byte-identically (O(1) point "
             "lookup by id, deterministic path 'chunks/<id>.json')");

    auto missing = ae::read_corpus_chunk_record(object_store, ref_store, mount, read_cap, "nonexistent");
    AE_CHECK(!missing.has_value(), "reading an id that was never written fails, not silently empty");

    // Malformed JSON round-trip: from_json rejects a record missing a required field.
    {
        std::vector<std::pair<std::string, ae::json::Value>> obj;
        obj.emplace_back("id", ae::json::Value::make_string("x"));
        // source_path deliberately omitted
        auto malformed = ae::corpus_chunk_record_from_json(ae::json::Value::make_object(std::move(obj)));
        AE_CHECK(!malformed.has_value() && malformed.error().code == "corpus_chunk.malformed_record",
                 "corpus_chunk_record_from_json() rejects a record missing a required string field, "
                 "matching memory_item_from_json()'s own 'rejects rather than guessing' contract");
    }

    // Malformed JSON round-trip (red-team finding, 2026-08-19): a missing/non-numeric line_start or
    // line_end used to silently default to 0 rather than being rejected, inconsistent with this same
    // function's own handling of id/source_path/source_file_hash.
    {
        std::vector<std::pair<std::string, ae::json::Value>> obj;
        obj.emplace_back("id", ae::json::Value::make_string("x"));
        obj.emplace_back("source_path", ae::json::Value::make_string("y"));
        // line_start deliberately omitted
        obj.emplace_back("line_end", ae::json::Value::make_number(5));
        obj.emplace_back("source_file_hash", ae::json::Value::make_string("h"));
        auto missing_line_start = ae::corpus_chunk_record_from_json(ae::json::Value::make_object(std::move(obj)));
        AE_CHECK(!missing_line_start.has_value() &&
                     missing_line_start.error().code == "corpus_chunk.malformed_record",
                 "corpus_chunk_record_from_json() rejects a record with a missing line_start, "
                 "instead of silently defaulting it to 0 (a fabricated, wrong citation line)");
    }
    {
        std::vector<std::pair<std::string, ae::json::Value>> obj;
        obj.emplace_back("id", ae::json::Value::make_string("x"));
        obj.emplace_back("source_path", ae::json::Value::make_string("y"));
        obj.emplace_back("line_start", ae::json::Value::make_number(1));
        obj.emplace_back("line_end", ae::json::Value::make_string("not-a-number"));  // wrong type
        obj.emplace_back("source_file_hash", ae::json::Value::make_string("h"));
        auto wrong_type_line_end = ae::corpus_chunk_record_from_json(ae::json::Value::make_object(std::move(obj)));
        AE_CHECK(!wrong_type_line_end.has_value() &&
                     wrong_type_line_end.error().code == "corpus_chunk.malformed_record",
                 "corpus_chunk_record_from_json() rejects a record whose line_end is present but "
                 "the wrong JSON type, not silently coerced or defaulted");
    }

    std::cout << (g_failures == 0 ? "test_corpus_chunk: OK\n" : "test_corpus_chunk: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
