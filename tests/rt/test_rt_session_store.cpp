// Proof for ADR-037 Phase 1: agentengine::rt::SessionStore (include/agentengine/rt/session_store.hpp),
// the host-injected persistence interface replacing quark::EventLog/FenceToken for AgentEngine's own
// runtime substrate. Deliberately no dependency on quark:: anywhere in this file. Covers:
//   - InMemorySessionStore round-trips bytes exactly (save then load returns identical bytes).
//   - load() on a session that was never saved fails cleanly with a contract error, not a crash.
//   - remove() then load() fails the same way as "never saved".
//   - remove() is idempotent (removing a never-saved id is not an error).
//   - exists() reflects save()/remove() correctly.
//   - FileSessionStore round-trips bytes to a REAL temp directory on disk, and survives being
//     reconstructed as a NEW FileSessionStore instance pointed at the same directory -- i.e. genuinely
//     persists across the C++ object's lifetime, not just in-process memory.
//   - FileSessionStore load() on a missing session fails cleanly; invalid ids (path separators, "..")
//     are rejected rather than escaping the configured root.

#include <cstdio>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "agentengine/rt/session_store.hpp"

using agentengine::error;
using agentengine::failure_class;
using agentengine::rt::FileSessionStore;
using agentengine::rt::InMemorySessionStore;
using agentengine::rt::SessionId;

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

std::vector<std::byte> bytes_from(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

std::string string_from(std::vector<std::byte> const& b) {
    std::string out(b.size(), '\0');
    for (std::size_t i = 0; i < b.size(); ++i) out[i] = static_cast<char>(b[i]);
    return out;
}

// `_getpid` (process.h) is the MSVC CRT spelling; POSIX has no leading underscore and lives in
// unistd.h. Only used to make each test run's temp-root name unique, matching
// test_real_filesystem_adapter.cpp's own precedent for this exact idiom.
[[nodiscard]] int current_pid() noexcept {
#if defined(_WIN32)
    return ::_getpid();
#else
    return ::getpid();
#endif
}

std::filesystem::path make_temp_root() {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("ae_rt_session_store_test_" + std::to_string(current_pid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);  // clean slate if a previous crashed run left it behind
    std::filesystem::create_directories(root);
    return root;
}

}  // namespace

int main() {
    // ---- InMemorySessionStore -------------------------------------------------------------------

    // M1: save() then load() round-trips the exact bytes.
    {
        InMemorySessionStore store;
        SessionId id = "session-alpha";
        auto saved = store.save(id, bytes_from("hello, session store"));
        check(saved.has_value(), "M1: save() succeeds");

        auto loaded = store.load(id);
        check(loaded.has_value(), "M1: load() succeeds after save()");
        check(loaded.has_value() && string_from(*loaded) == "hello, session store",
              "M1: load() returns byte-identical content to what was saved");
    }

    // M2: load() on a session that was never saved fails cleanly -- a proper result<T> error, never
    // a crash -- classified as a contract violation (unknown id), not a transient/fatal failure.
    {
        InMemorySessionStore store;
        auto loaded = store.load("never-saved-session");
        check(!loaded.has_value(), "M2: load() on an unsaved session id returns an error, not a value");
        check(loaded.has_value() || loaded.error().klass == failure_class::contract,
              "M2: the error is classified failure_class::contract");
        check(loaded.has_value() || loaded.error().code == "rt.session_store.not_found",
              "M2: the error code is rt.session_store.not_found");
    }

    // M3: remove() then load() fails the same way as never having saved it -- no distinction visible
    // through the read path (mirrors agent_session.hpp's own "no residue" property).
    {
        InMemorySessionStore store;
        SessionId id = "session-to-delete";
        check(store.save(id, bytes_from("will be removed")).has_value(),
              "M3: save() before removal succeeds");
        check(store.exists(id), "M3: exists() is true right after save()");

        auto removed = store.remove(id);
        check(removed.has_value(), "M3: remove() on an existing session succeeds");
        check(!store.exists(id), "M3: exists() is false right after remove()");

        auto loaded = store.load(id);
        check(!loaded.has_value(), "M3: load() after remove() returns an error");
        check(loaded.has_value() || loaded.error().code == "rt.session_store.not_found",
              "M3: load() after remove() reports the SAME not_found code as never-saved");
    }

    // M4: remove() is idempotent -- removing an id that was never saved is not an error.
    {
        InMemorySessionStore store;
        auto removed = store.remove("no-such-session-ever");
        check(removed.has_value(), "M4: remove() on a never-saved id succeeds (idempotent)");
    }

    // M5: exists() reflects the store's current state across save/overwrite/remove.
    {
        InMemorySessionStore store;
        SessionId id = "session-exists-check";
        check(!store.exists(id), "M5: exists() is false before any save()");
        check(store.save(id, bytes_from("v1")).has_value(), "M5: first save() succeeds");
        check(store.exists(id), "M5: exists() is true after save()");
        // Overwriting an existing id replaces its content rather than erroring or appending.
        check(store.save(id, bytes_from("v2")).has_value(), "M5: overwriting save() succeeds");
        auto loaded = store.load(id);
        check(loaded.has_value() && string_from(*loaded) == "v2",
              "M5: overwriting save() replaces the stored bytes, not appends");
    }

    // ---- FileSessionStore -------------------------------------------------------------------------

    std::filesystem::path root = make_temp_root();

    // F1: save() then load() round-trips exact bytes through a real file on disk.
    {
        FileSessionStore store(root);
        SessionId id = "file-session-one";
        auto saved = store.save(id, bytes_from("durable bytes on disk"));
        check(saved.has_value(), "F1: FileSessionStore::save() succeeds");

        auto loaded = store.load(id);
        check(loaded.has_value(), "F1: FileSessionStore::load() succeeds after save()");
        check(loaded.has_value() && string_from(*loaded) == "durable bytes on disk",
              "F1: FileSessionStore load() returns byte-identical content to what was saved");
        check(store.exists(id), "F1: FileSessionStore::exists() is true after save()");
    }

    // F2: genuine cross-instance persistence -- a NEW FileSessionStore object, constructed AFTER the
    // first one that wrote F1's data has already gone out of scope, still reads the same bytes back
    // from the same root directory. This is the load-bearing proof that this store is backed by real
    // durable storage, not just an in-process cache that happens to share a class name.
    {
        FileSessionStore reopened(root);
        auto loaded = reopened.load("file-session-one");
        check(loaded.has_value(),
              "F2: a freshly-constructed FileSessionStore instance finds data written by a PRIOR, "
              "now-destroyed instance pointed at the same root directory");
        check(loaded.has_value() && string_from(*loaded) == "durable bytes on disk",
              "F2: the reopened instance reads back byte-identical content");
    }

    // F3: load() on a session that was never saved fails cleanly with the same not_found contract.
    {
        FileSessionStore store(root);
        auto loaded = store.load("file-session-never-saved");
        check(!loaded.has_value(), "F3: FileSessionStore load() on a missing session returns an error");
        check(loaded.has_value() || loaded.error().klass == failure_class::contract,
              "F3: the error is classified failure_class::contract");
        check(loaded.has_value() || loaded.error().code == "rt.session_store.not_found",
              "F3: the error code is rt.session_store.not_found");
    }

    // F4: remove() then load() on FileSessionStore fails the same way, and the underlying file is
    // actually gone from disk (not merely "reported" gone).
    {
        FileSessionStore store(root);
        SessionId id = "file-session-to-delete";
        check(store.save(id, bytes_from("temporary")).has_value(), "F4: save() before removal succeeds");
        check(store.exists(id), "F4: exists() is true right after save()");

        auto removed = store.remove(id);
        check(removed.has_value(), "F4: remove() succeeds");
        check(!store.exists(id), "F4: exists() is false right after remove()");

        auto loaded = store.load(id);
        check(!loaded.has_value(), "F4: load() after remove() returns an error");
        check(loaded.has_value() || loaded.error().code == "rt.session_store.not_found",
              "F4: load() after remove() reports not_found, same as never-saved");

        // A brand new instance over the same root also sees it gone -- proves the file was actually
        // deleted from disk, not just hidden by some in-process cache.
        FileSessionStore reopened(root);
        check(!reopened.exists(id),
              "F4: a freshly-constructed instance over the same root also sees the session gone");
    }

    // F5: remove() on a never-saved id is idempotent (not an error), matching InMemorySessionStore.
    {
        FileSessionStore store(root);
        auto removed = store.remove("file-session-no-such-id-ever");
        check(removed.has_value(), "F5: FileSessionStore remove() on a never-saved id succeeds");
    }

    // F6: an id containing a path separator or '..' is rejected rather than silently escaping the
    // configured root directory.
    {
        FileSessionStore store(root);
        auto saved_slash = store.save("../escape-attempt", bytes_from("x"));
        check(!saved_slash.has_value(), "F6: save() rejects a session id containing '..'");
        check(saved_slash.has_value() || saved_slash.error().klass == failure_class::contract,
              "F6: the rejection is classified failure_class::contract");

        auto saved_backslash = store.save("nested\\path", bytes_from("x"));
        check(!saved_backslash.has_value(), "F6: save() rejects a session id containing a separator");

        check(!store.exists("../escape-attempt"), "F6: exists() degrades to false for an invalid id");
    }

    // F7: empty bytes round-trip too (a session that serializes to zero bytes is a legitimate, if
    // unusual, payload -- must not be confused with "file does not exist").
    {
        FileSessionStore store(root);
        SessionId id = "file-session-empty-payload";
        check(store.save(id, std::vector<std::byte>{}).has_value(), "F7: save() of empty bytes succeeds");
        check(store.exists(id), "F7: exists() is true for a session saved with empty bytes");
        auto loaded = store.load(id);
        check(loaded.has_value(), "F7: load() of an empty-payload session succeeds");
        check(loaded.has_value() && loaded->empty(), "F7: the loaded bytes are genuinely empty");
    }

    std::error_code cleanup_ec;
    std::filesystem::remove_all(root, cleanup_ec);  // best-effort cleanup, not part of the proof

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_session_store: ALL PASS\n");
    return 0;
}
