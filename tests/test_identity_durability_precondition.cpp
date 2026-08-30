// Real, adversarial, two-real-OS-process proof of gate item 6 in
// `docs/planning/content-durability-conformer-design-draft.md` §5: does making `Ledger`'s own content
// genuinely durable (`agentengine::FileWorktreeObjectStore`, this same round's own port) turn a
// currently-latent risk into a LIVE one, against CURRENT production code, if a host configures
// `Ledger`'s `durable_dir` without ALSO, consistently, configuring `IdentityAuthority::bootstrap()`'s
// own `durable_dir`?
//
// The precise mechanism (`identity-native-sandbox-worktree-design.md` §33's own original finding,
// fixed in §34.2 by making `IdentityAuthority` itself durable -- `identity_authority.hpp:103-105`):
// `Ledger`'s `blob_acl_`/`tree_acl_` are keyed by a bare `std::uint64_t` id, durably persisted alongside
// branch bookkeeping. `IdentityAuthority::mint_root()`'s own id allocation is a simple, in-process
// monotonic counter -- durable ONLY if `bootstrap()` was given a `durable_dir` on its OWN first call in
// that process. A fresh, in-memory-only `IdentityAuthority` in a NEW process always starts allocating
// ids from 1 again. Two DIFFERENT, entirely unrelated real principals, each the very first caller to
// `mint_root()` in their own separate process, therefore receive the IDENTICAL internal id (1) -- and
// `Ledger`'s own durable ACL bookkeeping, keyed on that bare id, cannot tell them apart.
//
// Before content was durable, this was a REAL but LATENT risk: a recovered ACL entry could point at a
// recycled id, but the CONTENT itself was unrecoverable (`ledger.get_blob_failed`), so nothing could
// actually be read through it. This proof demonstrates, on CURRENT production code, that adding real
// content durability is EXACTLY the change that converts this into a live, working cross-principal leak
// when `IdentityAuthority` is not ALSO configured durably -- and that configuring it correctly (the
// SAME `durable_dir` passed to `bootstrap()` in both processes) closes it, for real, not merely by
// argument.
//
//   [1] THE VULNERABLE CONFIGURATION (a real, live leak, reproduced on current production code): process
//       1 uses an IN-MEMORY-ONLY IdentityAuthority (no durable_dir -- the easy-to-get-wrong default) to
//       mint its owner identity (the FIRST mint_root() call in that process, so it gets id 1), commits
//       real secret content to a durable Ledger<FileWorktreeObjectStore>, and exits. Process 2 -- a
//       genuinely different, unrelated "attacker" -- ALSO uses an in-memory-only IdentityAuthority (no
//       durable_dir), and its OWN first mint_root() call ALSO receives id 1 (fresh in-memory allocation
//       always restarts at 1). The attacker's own, entirely-legitimately-minted identity handle
//       genuinely reads the original owner's real secret content via the ACL-gated get_blob_safe()
//       production path -- a real cross-principal leak, not a contrived direct-store bypass.
//   [2] THE CORRECT CONFIGURATION (fails closed, reproduced on the SAME production code): the identical
//       scenario, except BOTH processes configure IdentityAuthority::bootstrap() with the SAME durable
//       identity directory. Process 1's owner still gets id 1 (first-ever mint anywhere against this
//       durable identity store); process 2's "attacker" mint_root() call now correctly continues from
//       the DURABLE high-water-mark and receives a genuinely different id (2, never recycled) --
//       get_blob_safe() correctly, genuinely refuses the attacker's real, honestly-minted-but-different
//       identity with `ledger.blob_access_denied`.

#include "agentengine/core/file_worktree_object_store.hpp"
#include "agentengine/core/ledger.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

constexpr std::string_view kSecretContent = "this is owner's real, durable secret content";

// argv layout for every worker role: <role> <objects_dir> <ledger_dir> <result_file> [<identity_dir>]
// <identity_dir> is present ONLY in the "durable identity" ([2]) scenario -- its absence is exactly
// what selects the in-memory-only IdentityAuthority::bootstrap() call in [1]. Every role writes its own
// real result to <result_file> directly (plain file I/O, not shell redirection -- Windows cmd.exe's own
// quoting rules for a redirection operator combined with an already-doubled outer-quote self-relaunch
// command proved genuinely fragile when tried; direct file I/O sidesteps that entirely and is portable).

int run_owner_role(std::filesystem::path const& objects_dir, std::filesystem::path const& ledger_dir,
                    std::filesystem::path const& result_file,
                    std::optional<std::filesystem::path> const& identity_dir) {
    IdentityAuthority& authority =
        identity_dir ? IdentityAuthority::bootstrap(*identity_dir) : IdentityAuthority::bootstrap();
    IdentityHandle owner = authority.mint_root("real-owner");   // the FIRST mint in this fresh process

    auto storage_quota_r = agentengine::rt::AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
    if (!storage_quota_r.has_value()) return 1;

    Ledger<FileWorktreeObjectStore> ledger(FileWorktreeObjectStore(objects_dir), ledger_dir);
    auto root_r = drive(ledger.create_root_branch(owner, "identity-precondition"));
    if (!root_r.has_value()) return 1;

    auto blob_r = ledger.put_blob_safe(to_bytes(kSecretContent), owner);
    if (!blob_r.has_value()) return 1;

    Tree tree;
    tree.entries.push_back(TreeEntry{"secret.txt", *blob_r, false});
    auto commit_r = drive(ledger.commit(*root_r, tree, owner, *storage_quota_r));
    if (!commit_r.has_value()) return 1;

    std::ofstream out(result_file, std::ios::trunc);
    out << "digest=" << *blob_r << '\n' << "owner_id=" << owner.id() << '\n';
    return 0;
}

int run_attacker_role(std::filesystem::path const& objects_dir, std::filesystem::path const& ledger_dir,
                       std::filesystem::path const& result_file, std::string const& secret_digest,
                       std::optional<std::filesystem::path> const& identity_dir) {
    IdentityAuthority& authority =
        identity_dir ? IdentityAuthority::bootstrap(*identity_dir) : IdentityAuthority::bootstrap();
    IdentityHandle attacker = authority.mint_root("real-attacker");   // the FIRST mint in THIS fresh process

    Ledger<FileWorktreeObjectStore> ledger(FileWorktreeObjectStore(objects_dir), ledger_dir);
    auto blob_r = ledger.get_blob_safe(secret_digest, attacker);

    std::ofstream out(result_file, std::ios::trunc);
    out << "attacker_id=" << attacker.id() << '\n';
    if (blob_r.has_value()) {
        std::string content(reinterpret_cast<char const*>(blob_r->data()), blob_r->size());
        out << "leaked=" << (content == kSecretContent ? "genuine" : "garbage") << '\n';
        out << "content=" << content << '\n';
    } else {
        out << "leaked=denied\n" << "error_code=" << blob_r.error().code << '\n';
    }
    return 0;
}

[[nodiscard]] std::string run_child(std::filesystem::path const& self_exe, std::string const& args,
                                     std::filesystem::path const& result_file) {
    std::error_code ec;
    std::filesystem::remove(result_file, ec);
    std::string const inner = "\"" + self_exe.string() + "\" " + args;
    std::string const command = "\"" + inner + "\"";
    int const exit_code = std::system(command.c_str());
    if (exit_code != 0) return {};
    std::ifstream in(result_file);
    std::string output((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return output;
}

[[nodiscard]] std::string extract_field(std::string const& text, std::string const& key) {
    auto pos = text.find(key + "=");
    if (pos == std::string::npos) return {};
    pos += key.size() + 1;
    auto end = text.find('\n', pos);
    return text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::string_view(argv[1]) == "--owner-role") {
        std::optional<std::filesystem::path> identity_dir;
        if (argc >= 6) identity_dir = argv[5];
        return run_owner_role(argv[2], argv[3], argv[4], identity_dir);
    }
    if (argc >= 6 && std::string_view(argv[1]) == "--attacker-role") {
        std::optional<std::filesystem::path> identity_dir;
        if (argc >= 7) identity_dir = argv[6];
        return run_attacker_role(argv[2], argv[3], argv[4], argv[5], identity_dir);
    }

    if (argc < 1 || argv[0] == nullptr || std::string_view(argv[0]).empty()) {
        std::fprintf(stderr, "FAIL: argv[0] unavailable -- cannot self-relaunch\n");
        return EXIT_FAILURE;
    }
    std::filesystem::path const self_exe = std::filesystem::absolute(argv[0]);
    namespace fs = std::filesystem;

    // ---- [1] THE VULNERABLE CONFIGURATION: no identity_dir anywhere. -------------------------------
    {
        fs::path const objects_dir = fs::temp_directory_path() / "ae_test_identity_precondition_vuln_objects";
        fs::path const ledger_dir = fs::temp_directory_path() / "ae_test_identity_precondition_vuln_ledger";
        fs::path const owner_result = fs::temp_directory_path() / "ae_test_identity_precondition_vuln_owner.txt";
        fs::path const attacker_result =
            fs::temp_directory_path() / "ae_test_identity_precondition_vuln_attacker.txt";
        std::error_code ec;
        fs::remove_all(objects_dir, ec);
        fs::remove_all(ledger_dir, ec);

        std::string const owner_out =
            run_child(self_exe,
                      "--owner-role \"" + objects_dir.string() + "\" \"" + ledger_dir.string() + "\" \"" +
                          owner_result.string() + "\"",
                      owner_result);
        std::string const digest = extract_field(owner_out, "digest");
        check(!digest.empty(), "[1] setup: the owner role process exits cleanly and a real digest was "
                                "captured from its own result file");
        if (digest.empty()) return EXIT_FAILURE;

        std::string const attacker_out = run_child(
            self_exe,
            "--attacker-role \"" + objects_dir.string() + "\" \"" + ledger_dir.string() + "\" \"" +
                attacker_result.string() + "\" " + digest,
            attacker_result);
        std::string const leaked = extract_field(attacker_out, "leaked");
        check(leaked == "genuine",
              "[1] THE VULNERABLE CONFIGURATION: a genuinely different, unrelated \"attacker\" process, "
              "using its own honestly-minted (but recycled-id, since IdentityAuthority was NOT "
              "configured durably in either process) identity, successfully reads the REAL owner's "
              "real secret content through the ACL-gated get_blob_safe() production path -- "
              "leaked=genuine means the recovered content matched the exact real secret, not merely "
              "that access happened to be granted. This is the exact live risk docs/planning/content-"
              "durability-conformer-design-draft.md §5 gate item 6 named: content durability turns a "
              "previously-LATENT id-recycling risk into a REAL, working leak when IdentityAuthority is "
              "misconfigured.");
        std::fprintf(stderr, "[1] attacker result: %s\n", attacker_out.c_str());

        fs::remove_all(objects_dir, ec);
        fs::remove_all(ledger_dir, ec);
        fs::remove(owner_result, ec);
        fs::remove(attacker_result, ec);
    }

    // ---- [2] THE CORRECT CONFIGURATION: the SAME identity_dir, consistently, in both processes. ----
    {
        fs::path const objects_dir = fs::temp_directory_path() / "ae_test_identity_precondition_safe_objects";
        fs::path const ledger_dir = fs::temp_directory_path() / "ae_test_identity_precondition_safe_ledger";
        fs::path const identity_dir = fs::temp_directory_path() / "ae_test_identity_precondition_safe_identity";
        fs::path const owner_result = fs::temp_directory_path() / "ae_test_identity_precondition_safe_owner.txt";
        fs::path const attacker_result =
            fs::temp_directory_path() / "ae_test_identity_precondition_safe_attacker.txt";
        std::error_code ec;
        fs::remove_all(objects_dir, ec);
        fs::remove_all(ledger_dir, ec);
        fs::remove_all(identity_dir, ec);

        std::string const owner_out =
            run_child(self_exe,
                      "--owner-role \"" + objects_dir.string() + "\" \"" + ledger_dir.string() + "\" \"" +
                          owner_result.string() + "\" \"" + identity_dir.string() + "\"",
                      owner_result);
        std::string const digest = extract_field(owner_out, "digest");
        check(!digest.empty(), "[2] setup: the owner role process exits cleanly and a real digest was "
                                "captured");
        if (digest.empty()) return EXIT_FAILURE;

        std::string const attacker_out =
            run_child(self_exe,
                      "--attacker-role \"" + objects_dir.string() + "\" \"" + ledger_dir.string() + "\" \"" +
                          attacker_result.string() + "\" " + digest + " \"" + identity_dir.string() + "\"",
                      attacker_result);
        std::string const leaked = extract_field(attacker_out, "leaked");
        check(leaked == "denied",
              "[2] THE CORRECT CONFIGURATION: with IdentityAuthority::bootstrap() ALSO configured "
              "durably and consistently (the SAME identity_dir in both processes), the attacker's own "
              "honestly-minted identity now correctly, genuinely receives a DIFFERENT id (the durable "
              "high-water-mark is never recycled) -- get_blob_safe() correctly fails closed "
              "(leaked=denied), not a leak. Confirms the FIX for gate item 6's own named risk is a "
              "real, available host-configuration choice on current production code, not merely a "
              "theoretical mitigation.");
        check(extract_field(attacker_out, "error_code") == "ledger.blob_access_denied",
              "[2] the correctly-denied read fails with the precise expected error code, not a "
              "different or unexplained failure");
        std::fprintf(stderr, "[2] attacker result: %s\n", attacker_out.c_str());

        fs::remove_all(objects_dir, ec);
        fs::remove_all(ledger_dir, ec);
        fs::remove_all(identity_dir, ec);
        fs::remove(owner_result, ec);
        fs::remove(attacker_result, ec);
    }

    if (g_failures == 0) {
        std::printf("ALL CHECKS PASSED -- gate item 6 of docs/planning/content-durability-conformer-"
                     "design-draft.md §5, proven adversarially on CURRENT production code across two "
                     "genuinely separate OS processes: making Ledger content durable "
                     "(FileWorktreeObjectStore) DOES turn IdentityAuthority's own previously-latent "
                     "id-recycling risk into a real, working cross-principal content leak when a host "
                     "configures Ledger's durable_dir without ALSO, consistently, configuring "
                     "IdentityAuthority::bootstrap()'s own durable_dir -- and configuring both "
                     "consistently genuinely closes it, on this same real code.\n");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
