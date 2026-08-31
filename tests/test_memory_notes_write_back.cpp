// Proof for decisions/ADR-156-agent-notes-write-back.md -- GitHub issue #40's "agent.notes has ZERO
// implementation at all" gap, closed for the write-back half (the "ordinary writes" half needs no new
// code, see that ADR's own §2/file-top comment on memory_notes_materializer.hpp: agent.files' existing
// generic open()/file_write already works against any mounted path).
//
// Proves: a file written into the notes-inbox staging directory becomes a REAL, durable
// AgentAuthored MemoryItem, discoverable through the SAME rank_memory_items()/recall path
// test_memory_provider.cpp/test_memory_codeact_bridging.cpp already prove for other memory items --
// and that the notes-inbox subtree is genuinely isolated from write_memory_item()'s own structural
// <kind>/<id> storage area (no collision, no re-ingestion of the store's own internal records).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "agentengine/core/memory_provider.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "backends/native_jail/memory_notes_materializer.hpp"

using namespace agentengine;
using namespace agentengine::native_jail;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

void write_file(std::filesystem::path const& p, std::string const& content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}

}  // namespace

int main() {
    InMemoryWorktreeObjectStore object_store;
    rt::InMemoryAppendLogStore ref_store;
    Principal const principal{"p-notes-test", "tenant-a"};

    auto bootstrapped = ensure_memory_worktree(object_store, ref_store, principal);
    check(bootstrapped.has_value(), "setup: the memory worktree bootstraps");

    cap::FsWrite const inbox_cap{memory_notes_inbox_mount_id(principal), "", std::nullopt, std::nullopt};
    cap::FsWrite const structural_cap{memory_mount_id(principal), "", std::nullopt, std::nullopt};

    std::filesystem::path const scratch =
        std::filesystem::temp_directory_path() / "ae_memory_notes_write_back_test";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);

    // ---- R1: prepare -> write two real files -> harvest -> two real, durable MemoryItems ----------
    {
        std::filesystem::path const host_root = scratch / "r1";
        auto prepared = prepare_memory_notes_mount("notes", host_root, {"work", "input", "out"});
        check(prepared.has_value(), "R1: prepare_memory_notes_mount succeeds");
        if (!prepared) {
            std::fprintf(stderr, "  (R1 error: %s / %s)\n", prepared.error().code.c_str(),
                          prepared.error().message.c_str());
        }
        if (prepared) {
            check(prepared->first == "notes",
                  "R1: the returned mount name is the host-chosen presentation token");
            std::filesystem::path const staging_dir(prepared->second);
            check(std::filesystem::exists(staging_dir), "R1: a real, empty staging directory exists");

            // Simulate a script writing two note files (this test operates at the same
            // filesystem-adapter layer test_skill_mount_materializer.cpp/test_memory_codeact_bridging.cpp
            // already do, not through a live sandboxed worker).
            write_file(staging_dir / "todo.txt", "buy milk");
            write_file(staging_dir / "idea.txt", "ship agent.notes");

            auto items = harvest_memory_notes(object_store, ref_store, principal, inbox_cap,
                                                structural_cap, prepared->second, "run-7", "turn-2");
            check(items.has_value(), "R1: harvest_memory_notes succeeds");
            if (!items) {
                std::fprintf(stderr, "  (R1 harvest error: %s / %s)\n", items.error().code.c_str(),
                              items.error().message.c_str());
            }
            if (items) {
                check(items->size() == 2, "R1: two real files became two real MemoryItems");
                bool found_todo = false, found_idea = false;
                for (auto const& item : *items) {
                    check(item.origin.source == memory_source::agent_authored,
                          "R1: each item's origin.source is agent_authored, not user_stated/model_inferred");
                    check(item.origin.run_id == "run-7" && item.origin.turn_id == "turn-2",
                          "R1: each item carries the REAL run_id/turn_id the caller supplied, not a placeholder");
                    if (item.content == "buy milk") found_todo = true;
                    if (item.content == "ship agent.notes") found_idea = true;
                }
                check(found_todo && found_idea,
                      "R1: both files' REAL content round-tripped, not stubs");

                // ---- R2: the items are DURABLE -- discoverable through the same read path recall/
                // rank_memory_items already use, proving write_memory_item() really committed them
                // to the structural area, not just returned in-memory objects. ------------------------
                cap::FsRead const read_cap{memory_mount_id(principal), "", std::nullopt};
                Mount const structural_mount = memory_mount(principal);
                auto ranked = rank_memory_items(object_store, ref_store, structural_mount, read_cap,
                                                  "milk", 10);
                check(ranked.has_value(), "R2: rank_memory_items succeeds against the structural mount");
                if (!ranked) {
                    std::fprintf(stderr, "  (R2 error: %s / %s)\n", ranked.error().code.c_str(),
                                  ranked.error().message.c_str());
                }
                if (ranked) {
                    bool found = false;
                    for (auto const& item : *ranked) {
                        if (item.content == "buy milk") found = true;
                    }
                    check(found, "R2: the harvested note is discoverable through the SAME recall/"
                                  "rank_memory_items path every other memory item uses -- genuinely "
                                  "durable, not merely returned");
                }
            }
        }
    }

    // ---- R3: an empty staging directory harvests to zero items, no false positives -----------------
    {
        std::filesystem::path const host_root = scratch / "r3";
        auto prepared = prepare_memory_notes_mount("notes", host_root, {"work", "input", "out"});
        check(prepared.has_value(), "R3 setup: prepare_memory_notes_mount succeeds");
        if (prepared) {
            auto items = harvest_memory_notes(object_store, ref_store, principal, inbox_cap,
                                                structural_cap, prepared->second, "run-8", "turn-0");
            check(items.has_value() && items->empty(),
                  "R3: an empty staging directory harvests to zero MemoryItems, not a false positive");
        }
    }

    // ---- R4: reserved-mount-name collision fails closed, nothing written ---------------------------
    {
        std::filesystem::path const host_root = scratch / "r4";
        auto prepared = prepare_memory_notes_mount("notes", host_root, {"notes"});
        check(!prepared.has_value(), "R4: a reserved mount name collision is refused");
        if (!prepared) {
            check(prepared.error().code == "memory.mount_id_reserved",
                  "R4: the failure carries the specific, named reserved-collision error code");
        }
        check(!std::filesystem::exists(host_root),
              "R4: NOTHING was written to disk for this call -- fails closed before any directory "
              "creation");
    }

    // ---- R5: the inbox ref is isolated from the structural <kind>/<id> area (a genuinely separate
    // ref, not a subtree -- ADR-156's own first draft used a shared-ref subtree and a real failing
    // test caught the collision, see memory_notes_materializer.hpp's file-top comment) -- a SECOND
    // harvest run (a later turn) does NOT re-ingest the FIRST run's own already-committed structural
    // MemoryItem records as new notes. -----------------------------------------------------------
    {
        std::filesystem::path const host_root = scratch / "r5";
        auto prepared = prepare_memory_notes_mount("notes", host_root, {"work", "input", "out"});
        check(prepared.has_value(), "R5 setup: prepare_memory_notes_mount succeeds");
        if (prepared) {
            // Nothing written this "turn" -- the staging directory is fresh/empty (R3 already proves
            // this harvests to zero), but the PRINCIPAL's memory ref by now already has R1's two
            // structural MemoryItem records committed under <kind>/<id>. If the inbox and the
            // structural area shared a ref, harvest_mount()'s own recursive walk would have picked
            // those up as "files" too.
            auto items = harvest_memory_notes(object_store, ref_store, principal, inbox_cap,
                                                structural_cap, prepared->second, "run-9", "turn-0");
            check(items.has_value() && items->empty(),
                  "R5: a later turn's harvest sees zero items -- R1's own structural records were "
                  "never visible inside notes-inbox, proving genuine subtree isolation");
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_memory_notes_write_back: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_memory_notes_write_back: %d FAILURE(S)\n", g_failures);
    return 1;
}
