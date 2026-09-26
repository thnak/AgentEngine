// Proof for decisions/ADR-174-root-owned-container-seed.md (GitHub issue #68), offline half.
//
// ADR-171's `--cap-drop ALL` took CAP_DAC_OVERRIDE and CAP_DAC_READ_SEARCH away from the container's
// root, `docker cp <host path>` preserves the HOST file's ownership, and `write_verified()`
// materializes at mode 0600 -- so a seeded file was neither readable nor writable by the only user
// the container has. `docker cp -` reads a tar from stdin instead, and the ownership the container
// ends up with is whatever the ARCHIVE HEADERS say. `ustar_writer.hpp` writes those headers.
//
// This file proves the BYTES are right without needing a daemon, so it runs on every CI leg including
// the ones with no container runtime at all. `test_docker_seed_ownership.cpp` is the live other half.
//
//   U1 -- the archive is a whole number of 512-byte blocks and ends in two zero blocks. A tar reader
//         treats a stream without them as truncated.
//   U2 -- a regular file's header carries uid 0, gid 0, uname/gname "root", typeflag '0', mode 0600
//         and the exact byte count of its payload. THIS is the fix; everything else is scaffolding.
//   U3 -- the payload follows its header verbatim and is zero-padded to a block boundary, so the
//         next header starts where a reader expects it.
//   U4 -- the ustar magic/version are exactly "ustar\0" + "00". GNU tar's older dialect writes
//         "ustar  \0" here, and a reader that wants POSIX rejects it.
//   U5 -- the checksum is the one a reader recomputes: sum of the header with the checksum field
//         read as eight spaces. A wrong checksum is how a silently-corrupt header presents.
//   U6 -- a directory entry has typeflag '5', a trailing '/', mode 0700 and no payload; entries are
//         emitted in an order that always puts a directory before everything inside it.
//   U7 (negative control) -- a symlink is REFUSED, not followed and not skipped. A symlink seeded
//         into a container's workspace is a path-escape primitive.
//   U8 (negative control) -- the archive cap is enforced and names the entry it stopped at, with a
//         positive control that the same tree under an adequate cap succeeds.
//
// U9-U11 are REGRESSION GUARDS, and each one pins a defect this file's own first draft shipped and
// an adversarial review caught by executing it against a live daemon. Every case below is one that
// plain `docker cp <host path>` accepted before ADR-174, so refusing it was a pure regression -- and
// because the model's own code inside the sandbox can create these names, a refusal here would brick
// the surface for the rest of the session on the NEXT reset().
//
//   U9  -- `config..bak` and `..hidden` are ARCHIVED. The first draft rejected any name containing
//          ".." as a substring; one `cp notes.txt notes..bak` inside the sandbox was enough.
//   U10 -- a genuine ".." path COMPONENT is still refused (the positive control for U9: the check
//          got narrower, not absent).
//   U11 -- a path too long for ustar's 100-byte name / 155-byte prefix fields is written through a
//          PAX extended header rather than refused. The first draft refused it, which one
//          `npm install` would have hit. A long path that DOES have a legal prefix split still uses
//          the prefix field and emits no PAX header -- the control that PAX is used only where
//          needed.
//
// Needs no daemon, no network and no privileges.

#include "agentengine/sandbox/ustar_writer.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

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

constexpr std::size_t kBlock = agentengine::ustar::kBlockSize;

// Reads a NUL/space-terminated ustar field, which is how every textual field in the header is stored.
[[nodiscard]] std::string field(std::string const& archive, std::size_t header_offset,
                                std::size_t field_offset, std::size_t width) {
    std::string raw = archive.substr(header_offset + field_offset, width);
    std::size_t const end = raw.find_first_of(std::string("\0 ", 2));
    return end == std::string::npos ? raw : raw.substr(0, end);
}

// Finds the header block whose name field is exactly `name`, or npos. Walks block by block rather
// than assuming an order, so a test that cares about order asserts it explicitly (U6 does).
[[nodiscard]] std::size_t find_header(std::string const& archive, std::string const& name) {
    for (std::size_t off = 0; off + kBlock <= archive.size(); off += kBlock) {
        if (field(archive, off, 0, 100) == name) return off;
    }
    return std::string::npos;
}

// Finds the first header carrying `typeflag`, or npos.
[[nodiscard]] std::size_t find_typeflag(std::string const& archive, char typeflag) {
    for (std::size_t off = 0; off + kBlock <= archive.size(); off += kBlock) {
        if (archive[off + 156] == typeflag && archive.compare(off + 257, 5, "ustar") == 0) return off;
    }
    return std::string::npos;
}

[[nodiscard]] unsigned int recompute_checksum(std::string const& archive, std::size_t header_offset) {
    unsigned int sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        unsigned char const byte = (i >= 148 && i < 156)
                                       ? static_cast<unsigned char>(' ')
                                       : static_cast<unsigned char>(archive[header_offset + i]);
        sum += byte;
    }
    return sum;
}

void write_file(std::filesystem::path const& p, std::string const& content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

[[nodiscard]] std::filesystem::path make_temp_dir(std::string const& tag) {
    std::filesystem::path const dir = std::filesystem::temp_directory_path() / ("ae_ustar_" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

// Runs the writer into a string, which is what every check below inspects.
[[nodiscard]] agentengine::result<std::string> archive_of(
        std::filesystem::path const& dir,
        std::uint64_t cap = agentengine::ustar::kDefaultArchiveCapBytes) {
    std::ostringstream out(std::ios::binary);
    auto wrote = agentengine::ustar::write_archive_as_root(dir, out, cap);
    if (!wrote.has_value()) return std::unexpected(wrote.error());
    return out.str();
}

}  // namespace

int main() {
    std::printf("=== ADR-174 (issue #68): root-owned ustar seed archive, offline ===\n");

    // ---- U1-U6: the ordinary shape ---------------------------------------------------------------
    {
        std::filesystem::path const root = make_temp_dir("basic");
        write_file(root / "note.txt", "turn-1 content");
        write_file(root / "sub" / "deep.txt", "nested");

        auto archive = archive_of(root);
        check(archive.has_value(), "write_archive_as_root() succeeds on a regular tree");
        if (archive.has_value()) {
            std::string const& a = *archive;

            check(a.size() % kBlock == 0, "U1: the archive is a whole number of 512-byte blocks");
            check(a.size() >= 2 * kBlock &&
                      a.compare(a.size() - 2 * kBlock, 2 * kBlock, std::string(2 * kBlock, '\0')) == 0,
                  "U1: the archive ends in two zero blocks, so a reader does not see it as truncated");

            std::size_t const note = find_header(a, "note.txt");
            check(note != std::string::npos, "U2: the file's header is present under its relative name");
            if (note != std::string::npos) {
                check(field(a, note, 108, 8) == "0000000", "U2: uid is 0 -- the whole point of ADR-174");
                check(field(a, note, 116, 8) == "0000000", "U2: gid is 0");
                check(field(a, note, 265, 32) == "root", "U2: uname is root");
                check(field(a, note, 297, 32) == "root", "U2: gname is root");
                check(a[note + 156] == '0', "U2: typeflag is '0' (regular file)");
                check(field(a, note, 124, 12) == "00000000016",
                      "U2: size is the payload's exact byte count in octal (14 == 016)");
                check(field(a, note, 100, 8) == "0000600",
                      "U2: mode is 0600 -- the same bits write_verified() opens with, so the seed "
                      "changes ownership and nothing else");

                check(a.compare(note + kBlock, 14, "turn-1 content") == 0,
                      "U3: the payload follows its own header verbatim");
                check(a.compare(note + kBlock + 14, kBlock - 14, std::string(kBlock - 14, '\0')) == 0,
                      "U3: the payload is zero-padded to a block boundary");

                check(a.compare(note + 257, 6, std::string("ustar\0", 6)) == 0,
                      "U4: magic is POSIX \"ustar\\0\", not GNU's \"ustar  \\0\"");
                check(a.compare(note + 263, 2, "00") == 0, "U4: version is \"00\"");

                std::string const stored = field(a, note, 148, 8);
                char expected[8];
                std::snprintf(expected, sizeof(expected), "%06o", recompute_checksum(a, note));
                check(stored == expected,
                      "U5: the stored checksum is what a reader recomputes over the header");
            }

            std::size_t const dir = find_header(a, "sub/");
            check(dir != std::string::npos, "U6: the directory entry carries a trailing '/'");
            if (dir != std::string::npos) {
                check(a[dir + 156] == '5', "U6: typeflag is '5' (directory)");
                check(field(a, dir, 124, 12) == "00000000000", "U6: a directory has no payload");
                check(field(a, dir, 100, 8) == "0000700", "U6: mode is 0700 for a directory");
            }
            std::size_t const deep = find_header(a, "sub/deep.txt");
            check(deep != std::string::npos, "U6: the nested file is present at its relative path");
            check(dir != std::string::npos && deep != std::string::npos && dir < deep,
                  "U6: a directory is emitted before everything inside it");
            check(find_header(a, root.filename().generic_string() + "/") == std::string::npos,
                  "U6: the archive holds the directory's CONTENTS, not the directory itself");
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // ---- U7: a symlink is refused ----------------------------------------------------------------
    {
        std::filesystem::path const root = make_temp_dir("symlink");
        write_file(root / "real.txt", "real");
        std::error_code link_ec;
        std::filesystem::create_symlink("real.txt", root / "link.txt", link_ec);
        if (link_ec) {
            // Windows needs Developer Mode or elevation to create a symlink. Say so rather than
            // reporting a pass that never ran -- a check that silently degrades to "OK" is exactly
            // the anti-pattern CLAUDE.md's own positive-control rule exists to prevent.
            std::printf("[skip] U7: cannot create a symlink here (%s) -- negative control NOT run\n",
                        link_ec.message().c_str());
        } else {
            auto archive = archive_of(root);
            check(!archive.has_value(), "U7: a symlink in the tree is REFUSED, not followed or skipped");
            if (!archive.has_value()) {
                check(archive.error().code == "ustar_writer.unsupported_entry",
                      "U7: refused as a symlink specifically, with its own stable code");
            }
        }
        // The same, for a symlink pointing OUT of the tree. Worth its own case: the walk originally
        // used `std::filesystem::relative()`, which is weakly_canonical-based and FOLLOWS the link,
        // so this case reported a bogus "unsafe relative path: ../../etc/passwd" -- an operator would
        // have hunted for a path traversal that did not exist.
        std::filesystem::path const out_root = make_temp_dir("symlink_out");
        std::error_code out_ec;
        std::filesystem::create_symlink(std::filesystem::temp_directory_path() / "nowhere.txt",
                                        out_root / "escape.txt", out_ec);
        if (!out_ec) {
            auto archive = archive_of(out_root);
            check(!archive.has_value() && archive.error().code == "ustar_writer.unsupported_entry",
                  "U7: an OUT-OF-TREE symlink is refused as a symlink too, not as a bogus traversal");
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove_all(out_root, ec);
    }

    // ---- U8: the archive cap is enforced ---------------------------------------------------------
    {
        std::filesystem::path const root = make_temp_dir("cap");
        write_file(root / "big.bin", std::string(4096, 'x'));
        auto archive = archive_of(root, 1024);
        check(!archive.has_value(), "U8: an archive that would exceed its cap is refused");
        if (!archive.has_value()) {
            check(archive.error().code == "ustar_writer.archive_cap_exceeded",
                  "U8: refused with its own stable code");
            check(archive.error().message.find("big.bin") != std::string::npos,
                  "U8: the refusal names the entry it stopped at, so it is actionable");
        }
        auto ok = archive_of(root, 64 * 1024);
        check(ok.has_value(), "U8 (positive control): the same tree under an adequate cap succeeds");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // ---- U9: names containing ".." as a substring are ORDINARY names -----------------------------
    {
        std::filesystem::path const root = make_temp_dir("dotdot_ok");
        write_file(root / "config..bak", "a");
        write_file(root / "..hidden", "b");
        write_file(root / "trailing..", "c");
        auto archive = archive_of(root);
        check(archive.has_value(),
              "U9: a tree containing config..bak / ..hidden / trailing.. is archived, NOT refused "
              "-- docker cp accepted all three before ADR-174");
        if (archive.has_value()) {
            check(find_header(*archive, "config..bak") != std::string::npos,
                  "U9: config..bak is present under its real name");
            check(find_header(*archive, "..hidden") != std::string::npos,
                  "U9: ..hidden is present under its real name");
            // The claim under test is "every name the FILESYSTEM actually holds appears in the
            // archive", so it is asserted against a real directory listing rather than against the
            // names that were requested. Win32 silently strips trailing dots at creation -- and
            // `exists("trailing..")` then answers true for the stripped file, so even probing for
            // the requested name lies. Hard-coding either spelling would make this a platform
            // assertion wearing an archive assertion's clothes.
            bool every_real_name_present = true;
            std::string missing;
            for (auto const& de : std::filesystem::directory_iterator(root)) {
                std::string const on_disk = de.path().filename().generic_string();
                if (find_header(*archive, on_disk) == std::string::npos) {
                    every_real_name_present = false;
                    missing = on_disk;
                }
            }
            check(every_real_name_present,
                  every_real_name_present
                      ? "U9: every name this filesystem actually created appears in the archive"
                      : ("U9: \"" + missing + "\" exists on disk but is missing from the archive"));
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // ---- U10: a genuine ".." COMPONENT is still refused (positive control for U9) -----------------
    {
        check(agentengine::ustar::detail::has_unsafe_component("../evil.txt"),
              "U10: a leading \"..\" component is unsafe");
        check(agentengine::ustar::detail::has_unsafe_component("sub/../../evil.txt"),
              "U10: a \"..\" component in the middle is unsafe");
        check(agentengine::ustar::detail::has_unsafe_component("sub/.."),
              "U10: a trailing \"..\" component is unsafe");
        check(agentengine::ustar::detail::has_unsafe_component("/etc/passwd"),
              "U10: an absolute path is unsafe");
        check(agentengine::ustar::detail::has_unsafe_component(""), "U10: an empty name is unsafe");
        check(!agentengine::ustar::detail::has_unsafe_component("config..bak"),
              "U10: ..but \"config..bak\" is not -- the check is per-component, not a substring search");
        check(!agentengine::ustar::detail::has_unsafe_component("a/..b/c"),
              "U10: nor is a component merely STARTING with \"..\"");
    }

    // ---- U11: long paths go through PAX, not a refusal --------------------------------------------
    {
        std::filesystem::path const root = make_temp_dir("longpath");
        // A single 120-character component: longer than the 100-byte name field, and with no '/'
        // inside it there is no prefix split, so plain ustar genuinely cannot represent it.
        std::string const huge(120, 'a');
        write_file(root / huge, "x");
        auto archive = archive_of(root);
        check(archive.has_value(),
              "U11: a path with no legal ustar split is ARCHIVED via PAX, not refused -- docker cp "
              "accepted it before ADR-174");
        if (archive.has_value()) {
            std::size_t const pax = find_typeflag(*archive, 'x');
            check(pax != std::string::npos, "U11: a PAX extended header (typeflag 'x') is emitted");
            if (pax != std::string::npos) {
                std::string const record = archive->substr(pax + kBlock, 200);
                check(record.find("path=" + huge) != std::string::npos,
                      "U11: the PAX record carries the real path");
                // "<len> path=<name>\n", where <len> counts the whole record including its own digits.
                std::size_t const space = record.find(' ');
                std::size_t const declared = std::stoul(record.substr(0, space));
                std::string const expected = record.substr(0, declared);
                check(!expected.empty() && expected.back() == '\n',
                      "U11: the record's self-declared length is a fixed point -- it ends exactly at "
                      "its own terminating newline");
            }
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    {
        std::filesystem::path const root = make_temp_dir("splitpath");
        // 60 + 60 with a separator: too long for the name field alone, but a legal prefix split
        // exists, so this must use the prefix field and emit NO PAX header.
        std::string const dir_part(60, 'd');
        std::string const file_part(60, 'f');
        write_file(root / dir_part / file_part, "y");
        auto archive = archive_of(root);
        check(archive.has_value(), "U11 (control): a long path with a legal split is archived");
        if (archive.has_value()) {
            std::size_t const off = find_header(*archive, file_part);
            check(off != std::string::npos, "U11: the tail lands in the 100-byte name field");
            if (off != std::string::npos) {
                check(field(*archive, off, 345, 155) == dir_part,
                      "U11: the head lands in the 155-byte prefix field");
            }
            check(find_typeflag(*archive, 'x') == std::string::npos,
                  "U11: and NO PAX header is emitted -- PAX is used only where ustar cannot cope");
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // ---- U12: a non-ASCII name reaches the header as UTF-8 ---------------------------------------
    // Regression guard for a defect an adversarial review found by execution and no test covered.
    // The walk originally used `generic_string()`, which on Windows narrows through the process's
    // ACTIVE CODE PAGE: `path(L"cafe\u0301.txt")` came out as CP1252 bytes (invalid UTF-8 to every
    // tool inside the container), and a name the code page cannot represent at all threw
    // `std::system_error` straight out of a `result<>` function. Both cases below are ASCII-invisible
    // and would have passed every other check in this file.
    {
        std::filesystem::path const root = make_temp_dir("utf8");
        std::filesystem::path const accented(std::u8string(u8"caf\u00e9.txt"));
        std::filesystem::path const cjk(std::u8string(u8"\u6771\u4eac.txt"));
        write_file(root / accented, "accented");
        write_file(root / cjk, "cjk");

        auto archive = archive_of(root);
        check(archive.has_value(),
              archive.has_value()
                  ? "U12: a tree with non-ASCII names is archived, not thrown out of"
                  : ("U12 FAILED: " + archive.error().message));
        if (archive.has_value()) {
            // The expected bytes are stated literally, not derived from the same conversion under
            // test -- deriving them would make this check agree with the bug.
            check(find_header(*archive, "caf\xc3\xa9.txt") != std::string::npos,
                  "U12: the accented name is UTF-8 (c3 a9) in the header, not code-page bytes");
            check(find_header(*archive, "\xe6\x9d\xb1\xe4\xba\xac.txt") != std::string::npos,
                  "U12: a name outside any single-byte code page survives as UTF-8 rather than "
                  "throwing");
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // ---- U13: a directory the walk cannot enter is an ERROR, never a quiet partial archive -------
    // Two bugs live at this one spot, and this case is the only thing that separates them. The
    // range-for a reviewer found here called the THROWING `operator++`, so an unreadable subdirectory
    // terminated the process. Replacing it with `increment(ec)` and checking `ec` at the TOP of the
    // body then looked correct and was worse: a failed increment leaves the iterator equal to end, so
    // the loop simply exits and the check never runs -- the archive came back SUCCESSFUL carrying one
    // entry, having silently dropped everything the walk had not yet reached. A container view that
    // quietly differs from the ledger's is precisely what this file refuses symlinks to prevent.
    {
        std::filesystem::path const root = make_temp_dir("unreadable");
        write_file(root / "readable" / "a.txt", "visible");
        write_file(root / "locked" / "inner" / "b.txt", "hidden");
        std::error_code perm_ec;
        std::filesystem::permissions(root / "locked", std::filesystem::perms::none,
                                     std::filesystem::perm_options::replace, perm_ec);
        // Windows ACLs do not deny directory enumeration to the owner via these bits, and a test
        // running elevated bypasses them anyway -- so the guard is whether the denial actually took
        // effect here, probed rather than assumed from the platform.
        std::error_code probe_ec;
        std::filesystem::directory_iterator probe(root / "locked", probe_ec);
        if (perm_ec || !probe_ec) {
            std::printf("[skip] U13: this platform still lets the owner enumerate a 000 directory -- "
                        "negative control NOT run\n");
        } else {
            auto archive = archive_of(root);
            check(!archive.has_value(),
                  "U13: a directory the walk cannot enter fails the archive outright");
            if (!archive.has_value()) {
                check(archive.error().code == "ustar_writer.walk_failed",
                      "U13: reported as a walk failure, NOT as a successful partial archive");
            }
        }
        std::filesystem::permissions(root / "locked", std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, perm_ec);
        // Positive control: the same tree, readable, archives both branches -- so U13 is a specific
        // result and not a probe that always reports failure.
        auto ok = archive_of(root);
        check(ok.has_value() && find_header(*ok, "locked/inner/b.txt") != std::string::npos &&
                  find_header(*ok, "readable/a.txt") != std::string::npos,
              "U13 (positive control): the same tree, readable, archives every branch");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
