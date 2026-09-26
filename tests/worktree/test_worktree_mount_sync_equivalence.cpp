// Proof that `materialize_mount()`/`harvest_mount()` (src/backends/native_jail/worktree_mount_sync.hpp)
// do what they did before, only without re-deriving per file what they already hold.
//
// WHAT WAS WRONG.
//   materialize -- the walk held each file's digest, then called `mount_read()` for it anyway, which
//     re-read the ref's whole log and re-walked the tree from the root: Theta(n^2) name comparisons
//     and n log reads for a flat n-file mount. It now reads the digest in hand, through the same
//     capability and size-cap functions `mount_read()` uses.
//   harvest -- every file was SHA-256'd by `compute_digest()` for its ContentItem and then again by
//     `put_blob()` inside `mount_write()`. The digest now comes back from the write.
//
// This is a capability-gated read path, so the proof is differential against the removed code, kept
// verbatim below (`old_mount_read`, `old_materialize_subtree`, `old_harvest_subtree`), on a
// recording FileSystemAdapter that logs every operation in order:
//
//   M1 (positive control) -- the old materialize reads the ref log once per file: 200 reads for a
//         200-file mount. Without this, "1 read" below could mean a counter never wired up.
//   M2 (the guard) -- the new one reads it ONCE for the same mount, and writes the same files.
//   M3 -- 6000 random trees (nested, random contents) under random mounts and random read
//         capabilities (mount mismatch, path prefixes, size caps): wherever every name on the walk is
//         one valid, unique segment, old and new agree EXACTLY -- success or the same error code, and
//         the same ordered log of filesystem operations. Names drawn include "a/b", "..", ".", ""
//         and duplicates; for a tree containing one, the new walk may only REFUSE (never succeed where
//         the old one failed, never write anything the old one did not write first, byte for byte),
//         and the counts of each case are reported so neither class is vacuous. File CONTENTS are
//         compared, not only the operation log, which records sizes.
//   M4 -- 1000 random host directories harvested through random write capabilities (quota, file
//         count, prefix): old and new return the same ContentItems (digest, size, media type, order),
//         the same error code on failure, and leave the ref at the same tree.
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/worktree.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "backends/native_jail/worktree_mount_sync.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace agentengine;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

[[nodiscard]] std::vector<std::byte> bytes_of(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

// ---- The removed code, verbatim apart from names. -------------------------------------------------

template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<std::vector<std::byte>> old_mount_read(OS& object_store, RS& ref_store, Mount const& mount,
                                                              cap::FsRead const& granted,
                                                              std::string const& guest_path) {
    if (granted.mount_id != mount.mount_id) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability does not authorize the requested mount",
                                      "worktree.mount_capability_mismatch"});
    }
    if (!capability_detail::path_prefix_covers(granted.path_prefix, guest_path)) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability's path scope does not cover the requested path",
                                      "worktree.mount_path_outside_capability"});
    }

    auto full_segments = detail::combined_mount_segments(mount, guest_path);
    if (!full_segments) return std::unexpected(full_segments.error());

    auto ref = read_ref(ref_store, mount.ref_name);
    if (!ref) return std::unexpected(ref.error());
    if (!ref->has_value()) {
        return std::unexpected(error{failure_class::contract, "this mount's ref has never been committed",
                                      "worktree.mount_ref_missing"});
    }

    auto entry = detail::resolve_entry_at_path(object_store, (*ref)->tree_digest, *full_segments);
    if (!entry) return std::unexpected(entry.error());
    if (entry->is_tree) {
        return std::unexpected(error{failure_class::contract, "the requested path names a directory, not a file",
                                      "worktree.mount_read_is_directory"});
    }

    auto bytes = object_store.get_blob(entry->digest);
    if (!bytes) return std::unexpected(bytes.error());
    if (granted.size_cap_bytes.has_value() && bytes->size() > *granted.size_cap_bytes) {
        return std::unexpected(error{failure_class::policy,
                                      "the requested file exceeds this capability's size cap",
                                      "worktree.mount_read_exceeds_size_cap"});
    }
    return bytes;
}

template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<void> old_materialize_subtree(OS& object_store, RS& ref_store, Mount const& mount,
                                                    cap::FsRead const& granted, Digest const& tree_digest,
                                                    std::string const& guest_prefix, FileSystemAdapter& fs) {
    auto tree = object_store.get_tree(tree_digest);
    if (!tree) return std::unexpected(tree.error());
    for (auto const& entry : tree->entries) {
        std::string guest_path = guest_prefix.empty() ? entry.name : guest_prefix + "/" + entry.name;
        if (entry.is_tree) {
            auto mk = fs.make_directory(guest_path, /*parents=*/true);
            if (!mk) return std::unexpected(mk.error());
            auto r = old_materialize_subtree(object_store, ref_store, mount, granted, entry.digest, guest_path, fs);
            if (!r) return r;
            continue;
        }
        auto bytes = old_mount_read(object_store, ref_store, mount, granted, guest_path);
        if (!bytes) return std::unexpected(bytes.error());
        auto written = fs.write_file(guest_path, *bytes, /*append=*/false);
        if (!written) return std::unexpected(written.error());
    }
    return {};
}

template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<void> old_materialize_mount(OS& object_store, RS& ref_store, Mount const& mount,
                                                  cap::FsRead const& granted, FileSystemAdapter& fs) {
    auto ref = read_ref(ref_store, mount.ref_name);
    if (!ref) return std::unexpected(ref.error());
    if (!ref->has_value()) {
        return std::unexpected(error{failure_class::contract, "this mount's ref has never been committed",
                                      "worktree.mount_ref_missing"});
    }
    auto subtree_digest =
        agentengine::detail::resolve_subtree_digest(object_store, (*ref)->tree_digest, mount.subtree_path);
    if (!subtree_digest) return std::unexpected(subtree_digest.error());
    return old_materialize_subtree(object_store, ref_store, mount, granted, *subtree_digest, "", fs);
}

template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<void> old_harvest_subtree(OS& object_store, RS& ref_store, Mount const& mount,
                                                cap::FsWrite const& granted, std::string const& guest_prefix,
                                                FileSystemAdapter& fs, std::vector<ContentItem>& out) {
    auto entries = fs.list_directory(guest_prefix);
    if (!entries) return std::unexpected(entries.error());
    for (auto const& entry : *entries) {
        std::string guest_path = guest_prefix.empty() ? entry.name : guest_prefix + "/" + entry.name;
        if (entry.is_directory) {
            auto r = old_harvest_subtree(object_store, ref_store, mount, granted, guest_path, fs, out);
            if (!r) return r;
            continue;
        }
        auto bytes = fs.read_file(guest_path);
        if (!bytes) return std::unexpected(bytes.error());
        auto digest = compute_digest(*bytes);
        if (!digest) return std::unexpected(digest.error());
        auto written = mount_write(object_store, ref_store, mount, granted, guest_path, *bytes);
        if (!written) return std::unexpected(written.error());

        std::string media_type = native_jail::detail::guess_media_type(entry.name);
        out.push_back(ContentItem{
            Media{BlobRef{*digest, media_type, bytes->size(), "worktree"}, media_type},
            content_origin::tool,
            /*tainted=*/true});
    }
    return {};
}

// ---- Harness. --------------------------------------------------------------------------------------

// Counts read_from() -- one per read_ref().
class CountingLogStore {
public:
    [[nodiscard]] result<rt::SeqNo> append(rt::LogId const& id, std::vector<std::byte> bytes) {
        return inner_.append(id, std::move(bytes));
    }
    [[nodiscard]] result<std::vector<std::vector<std::byte>>> read_from(rt::LogId const& id, rt::SeqNo from) const {
        ++reads;
        return inner_.read_from(id, from);
    }
    [[nodiscard]] rt::SeqNo last_seq(rt::LogId const& id) const { return inner_.last_seq(id); }
    mutable std::size_t reads = 0;

private:
    rt::InMemoryAppendLogStore inner_;
};

// An in-memory host directory that logs every operation, in order, and refuses the path shapes a real
// adapter refuses (absolute, empty segment, '.', '..') and a write into a missing parent.
class RecordingFs final : public FileSystemAdapter {
public:
    std::vector<std::string> log;
    std::map<std::string, std::vector<std::byte>> files;
    std::set<std::string> dirs{""};

    static bool valid(std::string_view p) {
        if (p.empty()) return false;
        if (p.front() == '/') return false;
        std::size_t start = 0;
        while (true) {
            std::size_t const slash = p.find('/', start);
            std::string_view const seg = p.substr(start, slash == std::string_view::npos ? p.npos : slash - start);
            if (seg.empty() || seg == "." || seg == "..") return false;
            if (slash == std::string_view::npos) return true;
            start = slash + 1;
        }
    }
    static std::string parent_of(std::string const& p) {
        auto const slash = p.find_last_of('/');
        return slash == std::string::npos ? std::string{} : p.substr(0, slash);
    }
    static error refused(std::string_view p) {
        return error{failure_class::contract, "refused path '" + std::string(p) + "'", "test_fs.refused"};
    }

    result<std::vector<std::byte>> read_file(std::string_view path) override {
        log.push_back("read " + std::string(path));
        auto it = files.find(std::string(path));
        if (it == files.end()) return std::unexpected(refused(path));
        return it->second;
    }
    result<void> write_file(std::string_view path, std::span<std::byte const> data, bool append) override {
        std::string const p(path);
        log.push_back("write " + p + " " + std::to_string(data.size()) + (append ? " append" : ""));
        if (!valid(path) || !dirs.contains(parent_of(p)) || dirs.contains(p)) return std::unexpected(refused(path));
        files[p].assign(data.begin(), data.end());
        return {};
    }
    result<void> make_directory(std::string_view path, bool parents) override {
        std::string const p(path);
        log.push_back("mkdir " + p + (parents ? " -p" : ""));
        if (!valid(path) || files.contains(p)) return std::unexpected(refused(path));
        for (std::string cur = p; !cur.empty(); cur = parent_of(cur)) {
            if (files.contains(cur)) return std::unexpected(refused(path));
            dirs.insert(cur);
        }
        return {};
    }
    result<std::vector<DirEntry>> list_directory(std::string_view path) override {
        std::string const p(path);
        log.push_back("ls " + p);
        if (!dirs.contains(p)) return std::unexpected(refused(path));
        std::vector<DirEntry> out;
        std::string const prefix = p.empty() ? "" : p + "/";
        for (auto const& d : dirs) {
            if (!d.empty() && d.starts_with(prefix) && d.find('/', prefix.size()) == std::string::npos) {
                out.push_back(DirEntry{d.substr(prefix.size()), true, 0});
            }
        }
        for (auto const& [f, bytes] : files) {
            if (f.starts_with(prefix) && f.find('/', prefix.size()) == std::string::npos) {
                out.push_back(DirEntry{f.substr(prefix.size()), false, bytes.size()});
            }
        }
        std::ranges::sort(out, {}, &DirEntry::name);
        return out;
    }
    result<void> remove(std::string_view, bool) override { return std::unexpected(refused("remove")); }
    result<void> rename(std::string_view, std::string_view) override { return std::unexpected(refused("rename")); }
    result<void> copy_file(std::string_view, std::string_view) override { return std::unexpected(refused("copy")); }
    result<bool> exists(std::string_view path) override {
        std::string const p(path);
        return files.contains(p) || dirs.contains(p);
    }
    result<std::string> canonicalize(std::string_view path) override { return std::string(path); }
};

[[nodiscard]] std::string status_of(result<void> const& r) { return r.has_value() ? "ok" : r.error().code; }

// A random tree. Returns whether any name on it is not one valid, unique segment.
struct Built {
    Digest digest;
    bool ambiguous = false;
};

Built random_tree(InMemoryWorktreeObjectStore& store, std::mt19937& rng, int depth) {
    static std::vector<std::string> const clean = {"a", "b", "c", "src", "x.txt", "y.json"};
    static std::vector<std::string> const odd = {"a/b", "..", ".", "", "/abs", "b/"};
    Tree tree;
    bool ambiguous = false;
    int const n = std::uniform_int_distribution<int>(0, 4)(rng);
    std::set<std::string> seen;
    for (int i = 0; i < n; ++i) {
        std::string name;
        int const kind = std::uniform_int_distribution<int>(0, 19)(rng);
        if (kind == 0) {
            name = odd[std::uniform_int_distribution<std::size_t>(0, odd.size() - 1)(rng)];
        } else {
            name = clean[std::uniform_int_distribution<std::size_t>(0, clean.size() - 1)(rng)];
        }
        bool const is_tree = depth < 3 && std::uniform_int_distribution<int>(0, 2)(rng) == 0;
        Digest d;
        if (is_tree) {
            auto sub = random_tree(store, rng, depth + 1);
            d = sub.digest;
            ambiguous = ambiguous || sub.ambiguous;
        } else {
            std::string content(std::uniform_int_distribution<std::size_t>(0, 12)(rng), 'z');
            content += std::to_string(std::uniform_int_distribution<int>(0, 3)(rng));
            d = *store.put_blob(bytes_of(content));
        }
        auto segs = split_mount_path(name);
        if (!segs.has_value() || segs->size() != 1 || seen.contains(name)) ambiguous = true;
        seen.insert(name);
        tree.entries.push_back(TreeEntry{name, d, is_tree});
    }
    return Built{*store.put_tree(std::move(tree)), ambiguous};
}

[[nodiscard]] bool is_prefix(std::vector<std::string> const& shorter, std::vector<std::string> const& longer) {
    return shorter.size() <= longer.size() && std::equal(shorter.begin(), shorter.end(), longer.begin());
}

}  // namespace

int main() {
    // ---- M1/M2: log reads for a flat 200-file mount.
    {
        InMemoryWorktreeObjectStore store;
        CountingLogStore refs;
        (void)commit_ref(refs, "session:flat", *store.put_tree(Tree{}));
        Mount const mount{"/work", "session:flat", ""};
        cap::FsWrite const w{"/work", "", std::nullopt, std::nullopt};
        for (int i = 0; i < 200; ++i) {
            (void)mount_write(store, refs, mount, w, "f" + std::to_string(i) + ".txt", bytes_of("content " + std::to_string(i)));
        }
        cap::FsRead const r{"/work", "", std::nullopt};

        RecordingFs old_fs;
        refs.reads = 0;
        auto old_r = old_materialize_mount(store, refs, mount, r, old_fs);
        std::size_t const old_reads = refs.reads;

        RecordingFs new_fs;
        refs.reads = 0;
        auto new_r = native_jail::materialize_mount(store, refs, mount, r, new_fs);
        std::size_t const new_reads = refs.reads;

        check(old_r.has_value() && old_reads == 201,
              "M1 (positive control): the old materialize reads the ref log 201 times for 200 files, got " +
                  std::to_string(old_reads));
        check(new_r.has_value() && new_reads == 1,
              "M2 (the guard): the new one reads it once, got " + std::to_string(new_reads));
        check(old_fs.log == new_fs.log && old_fs.files == new_fs.files && new_fs.files.size() == 200,
              "M2: ... and performs the same 200 writes, in the same order, with the same bytes");
    }

    std::mt19937 rng(20260914);  // fixed: a failure must be reproducible

    // ---- M3: materialize, differential.
    {
        int agree_ok = 0;
        int agree_err = 0;
        int ambiguous_same = 0;
        int ambiguous_refused_only_new = 0;
        int violations = 0;
        std::string first;
        std::vector<std::string> const prefixes = {"", "a", "b", "a/b", "src", "x.txt"};
        std::vector<std::string> const subtrees = {"", "", "", "a", "src", "missing"};
        for (int t = 0; t < 6000; ++t) {
            InMemoryWorktreeObjectStore store;
            rt::InMemoryAppendLogStore refs;
            auto const built = random_tree(store, rng, 0);
            (void)commit_ref(refs, "session:m3", built.digest);
            Mount const mount{"/work", "session:m3", subtrees[std::uniform_int_distribution<std::size_t>(0, 5)(rng)]};
            std::optional<std::uint64_t> size_cap;
            if (int const c = std::uniform_int_distribution<int>(0, 3)(rng); c > 0) size_cap = std::uint64_t(c * 5 - 5);
            cap::FsRead const granted{std::uniform_int_distribution<int>(0, 9)(rng) == 0 ? "/other" : "/work",
                                      prefixes[std::uniform_int_distribution<std::size_t>(0, 5)(rng)], size_cap};

            RecordingFs old_fs;
            RecordingFs new_fs;
            std::string const old_s = status_of(old_materialize_mount(store, refs, mount, granted, old_fs));
            std::string const new_s = status_of(native_jail::materialize_mount(store, refs, mount, granted, new_fs));
            // The log records sizes, not bytes, so the files themselves are compared too: a red-team
            // mutant that corrupted one byte of every nested file passed an earlier, log-only version.
            bool const same = old_s == new_s && old_fs.log == new_fs.log && old_fs.files == new_fs.files;

            // Ambiguity only matters if the walk reached the subtree at all; a tree flagged ambiguous
            // may still agree exactly (the odd name sat outside the mount, or the walk failed first).
            if (same) {
                if (built.ambiguous) {
                    ++ambiguous_same;
                } else {
                    (old_s == "ok" ? agree_ok : agree_err) += 1;
                }
                continue;
            }
            bool const new_files_are_old_files = std::ranges::all_of(new_fs.files, [&](auto const& kv) {
                auto it = old_fs.files.find(kv.first);
                return it != old_fs.files.end() && it->second == kv.second;
            });
            bool const allowed = built.ambiguous && new_s != "ok" &&
                                 (new_s == "worktree.mount_path_malformed" || new_s == "worktree.mount_path_ambiguous" ||
                                  new_s == "worktree.mount_path_absolute") &&
                                 is_prefix(new_fs.log, old_fs.log) && new_files_are_old_files;
            if (allowed) {
                ++ambiguous_refused_only_new;
                continue;
            }
            if (violations++ == 0) {
                first = "trial " + std::to_string(t) + ": old " + old_s + " (" + std::to_string(old_fs.log.size()) +
                        " ops), new " + new_s + " (" + std::to_string(new_fs.log.size()) + " ops), ambiguous=" +
                        (built.ambiguous ? "yes" : "no");
            }
        }
        std::printf("[info] M3: %d clean agree ok, %d clean agree on an error, %d odd-name trees agree exactly, "
                    "%d odd-name trees refused only by the new walk\n",
                    agree_ok, agree_err, ambiguous_same, ambiguous_refused_only_new);
        check(agree_ok > 500 && agree_err > 500 && ambiguous_refused_only_new > 50,
              "M3: every class is populated (successes, shared errors, and odd-name refusals)");
        check(violations == 0,
              "M3 (the guard): on 6000 random trees, old and new materialize agree exactly except where an odd "
              "name makes the new walk refuse, having written only a prefix of what the old one wrote" +
                  (violations == 0 ? std::string{} : " -- " + std::to_string(violations) + " violations, first " + first));
    }

    // ---- M4: harvest, differential.
    {
        int agree_ok = 0;
        int agree_err = 0;
        int mismatches = 0;
        std::string first;
        std::vector<std::string> const names = {"a.txt", "b.json", "c", "img.png", "notes.csv"};
        std::vector<std::string> const dirs = {"", "", "sub", "sub/deep", "out"};
        std::vector<std::string> const prefixes = {"", "", "sub", "out", "a.txt"};
        for (int t = 0; t < 1000; ++t) {
            RecordingFs host;
            int const n = std::uniform_int_distribution<int>(0, 6)(rng);
            for (int i = 0; i < n; ++i) {
                std::string const dir = dirs[std::uniform_int_distribution<std::size_t>(0, 4)(rng)];
                if (!dir.empty()) (void)host.make_directory(dir, true);
                std::string const path =
                    (dir.empty() ? "" : dir + "/") + names[std::uniform_int_distribution<std::size_t>(0, 4)(rng)];
                if (host.dirs.contains(path)) continue;
                std::string content(std::uniform_int_distribution<std::size_t>(0, 30)(rng), 'h');
                content += std::to_string(i % 3);
                (void)host.write_file(path, bytes_of(content), false);
            }
            std::optional<std::uint64_t> quota;
            if (std::uniform_int_distribution<int>(0, 2)(rng) == 0) quota = std::uint64_t(std::uniform_int_distribution<int>(0, 80)(rng));
            std::optional<std::uint32_t> count;
            if (std::uniform_int_distribution<int>(0, 2)(rng) == 0) count = std::uint32_t(std::uniform_int_distribution<int>(0, 4)(rng));
            cap::FsWrite const granted{"/out", prefixes[std::uniform_int_distribution<std::size_t>(0, 4)(rng)], quota, count};
            Mount const mount{"/out", "session:m4", std::uniform_int_distribution<int>(0, 1)(rng) == 0 ? "" : "art"};

            auto run = [&](bool use_old, std::vector<ContentItem>& items, Digest& final_tree) -> std::string {
                InMemoryWorktreeObjectStore store;
                rt::InMemoryAppendLogStore refs;
                (void)commit_ref(refs, "session:m4", *store.put_tree(Tree{}));
                RecordingFs fs = host;
                std::string status;
                if (use_old) {
                    status = status_of(old_harvest_subtree(store, refs, mount, granted, "", fs, items));
                } else {
                    auto r = native_jail::harvest_mount(store, refs, mount, granted, fs);
                    if (r) items = std::move(*r);
                    status = r.has_value() ? "ok" : r.error().code;
                }
                auto ref = read_ref(refs, "session:m4");
                final_tree = (ref && ref->has_value()) ? (*ref)->tree_digest : Digest{};
                return status;
            };
            std::vector<ContentItem> old_items;
            std::vector<ContentItem> new_items;
            Digest old_tree;
            Digest new_tree;
            std::string const old_s = run(true, old_items, old_tree);
            std::string const new_s = run(false, new_items, new_tree);

            auto describe = [](std::vector<ContentItem> const& items) {
                std::string s;
                for (auto const& it : items) {
                    auto const& media = std::get<Media>(it.value);
                    auto const& blob = std::get<BlobRef>(media.payload);
                    s += blob.digest + ":" + std::to_string(blob.size) + ":" + blob.media_type + ":" + blob.store + ":" +
                         media.media_type + ":" + std::to_string(static_cast<int>(it.origin)) +
                         (it.tainted ? ":tainted;" : ";");
                }
                return s;
            };
            // On failure the old code discards its partial item list with the error; so does the new.
            bool const same = old_s == new_s && old_tree == new_tree &&
                              (old_s != "ok" || describe(old_items) == describe(new_items));
            if (same) {
                (old_s == "ok" ? agree_ok : agree_err) += 1;
            } else if (mismatches++ == 0) {
                first = "trial " + std::to_string(t) + ": old " + old_s + ", new " + new_s;
            }
        }
        check(agree_ok > 100 && agree_err > 100,
              "M4: harvests both succeed (" + std::to_string(agree_ok) + ") and fail (" + std::to_string(agree_err) + ")");
        check(mismatches == 0,
              "M4: on 1000 random host directories, old and new harvest return the same ContentItems, the same "
              "error, and leave the ref at the same tree" +
                  (mismatches == 0 ? std::string{} : " -- " + std::to_string(mismatches) + " mismatches, first " + first));
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
