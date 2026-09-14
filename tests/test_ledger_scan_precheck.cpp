// Proof that `Ledger::would_accept_blob_write()` -- the dry run `RealIoFileSystem::scan_and_drain_into_tree()`
// uses to refuse a whole scan BEFORE writing any of it -- predicts exactly what `put_blob_safe()` then
// does, on every ledger, not only on one built with the default ACL-root cap.
//
// WHAT WAS WRONG. The dry run restated the admission rule by hand and drifted from it twice:
//   - it compared against the compile-time default cap (64) instead of the ledger's configured one,
//     so a ledger with a lower cap approved a scan that the real write then refused partway through,
//     leaving the files before it stored with ACL roots and no tree referencing them -- the outcome the
//     dry run exists to prevent -- and a ledger with a higher cap refused scans it would accept;
//   - it ignored `mark_digest_shared()`, the documented escape hatch for a digest that has hit the cap,
//     so a scan containing such a file was still refused.
// Both now go through one `acl_root_admissible()`.
//
//   S1 (positive control) -- the dry run is not vacuous: on a default-cap ledger, a scan containing a
//         file whose digest already has 64 other roots is refused by the pre-check, and the scan's
//         other file is not written.
//   S2 -- a configured cap of 1: the same refusal comes from the pre-check (its own message, whichever
//         file the directory walk visits first), and the scan's other file gets no ACL root.
//   S3 -- a configured cap above the default: a digest with 64 other roots is still admissible, and
//         the scan succeeds.
//   S4 -- the escape hatch: a digest saturated at the cap and then marked shared is scannable.
//   S5 (the guard) -- across caps 0, 1, 2, 3 and 64, a random sequence of 4000 writes by 6 identities
//         of 5 contents, with random `mark_digest_shared()` calls mixed in: before every write the dry
//         run's answer equals whether `put_blob_safe()` then succeeds. The number of refusals seen is
//         reported, so agreement is not agreement on "always yes".
//
// Needs no daemon, no network and no credentials.

#include "agentengine/core/ledger.hpp"
#include "agentengine/sandbox/real_io_filesystem.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace agentengine;
namespace fs = std::filesystem;

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

template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

[[nodiscard]] fs::path fresh_dir(std::string const& name) {
    fs::path const p = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    return p;
}

// Gives `bytes` `n` distinct roots other than anyone the caller will scan as.
void saturate(Ledger<>& ledger, std::string const& bytes, int n, std::string const& label) {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    for (int i = 0; i < n; ++i) {
        IdentityHandle other = authority.mint_root(label + "-" + std::to_string(i));
        (void)ledger.put_blob_safe(to_bytes(bytes), other);
    }
}

// Stages {a_fresh.txt, z_contested.txt} and scans them as `scanner`. The uncontested file sorts first,
// so where the walk is alphabetical (NTFS) a scan refused only at the second write has already
// written the first -- which is what S1/S2's "not written" checks look for.
[[nodiscard]] agentengine::result<Tree> scan_two(Ledger<>& ledger, fs::path const& root, IdentityHandle scanner) {
    RealIoFileSystem io(root);
    (void)io.write("a_fresh.txt", to_bytes("fresh content only this scan writes"));
    (void)io.write("z_contested.txt", to_bytes("contested content"));
    return drive(io.scan_and_drain_into_tree(ledger, scanner));
}

// True when the refusal came from the whole-scan pre-check rather than from a write partway through.
[[nodiscard]] bool refused_by_precheck(agentengine::result<Tree> const& r) {
    return !r.has_value() && r.error().code == "ledger.acl_root_cap_exceeded" &&
           r.error().message.find("before writing any of it") != std::string::npos;
}

}  // namespace

int main() {
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    auto const fresh_digest = compute_digest(to_bytes("fresh content only this scan writes"));
    if (!fresh_digest.has_value()) {
        std::printf("[FAIL] compute_digest failed\n");
        return 1;
    }

    // ---- S1: positive control, default cap.
    {
        IdentityHandle scanner = authority.mint_root("precheck-s1-scanner");
        fs::path const root = fresh_dir("ae_precheck_s1");
        Ledger<> ledger;
        saturate(ledger, "contested content", static_cast<int>(Ledger<>::kMaxAclRootsPerDigest), "precheck-s1");
        auto r = scan_two(ledger, root, scanner);
        check(refused_by_precheck(r),
              "S1 (positive control): default cap, a digest with 64 other roots -- the pre-check refuses the scan" +
                  (r.has_value() ? std::string{" (it succeeded)"} : " (got: " + r.error().message + ")"));
        check(!ledger.get_blob_safe(*fresh_digest, scanner).has_value(),
              "S1: ... and the scan's other file was not written");
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // ---- S2: configured cap below the default.
    {
        IdentityHandle scanner = authority.mint_root("precheck-s2-scanner");
        fs::path const root = fresh_dir("ae_precheck_s2");
        Ledger<> ledger(InMemoryWorktreeObjectStore{}, std::nullopt, /*max_acl_roots_per_digest=*/1);
        saturate(ledger, "contested content", 1, "precheck-s2");
        auto r = scan_two(ledger, root, scanner);
        check(refused_by_precheck(r),
              "S2: configured cap 1, a digest with 1 other root -- the PRE-CHECK refuses the scan, not a write "
              "partway through" +
                  (r.has_value() ? std::string{" (it succeeded)"} : " (got: " + r.error().message + ")"));
        check(!ledger.get_blob_safe(*fresh_digest, scanner).has_value(),
              "S2: ... and the scan's other file has no ACL root (nothing written before the refusal)");
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // ---- S3: configured cap above the default.
    {
        IdentityHandle scanner = authority.mint_root("precheck-s3-scanner");
        fs::path const root = fresh_dir("ae_precheck_s3");
        Ledger<> ledger(InMemoryWorktreeObjectStore{}, std::nullopt, /*max_acl_roots_per_digest=*/100);
        saturate(ledger, "contested content", static_cast<int>(Ledger<>::kMaxAclRootsPerDigest), "precheck-s3");
        auto r = scan_two(ledger, root, scanner);
        check(r.has_value() && r->entries.size() == 2,
              "S3: configured cap 100, a digest with 64 other roots -- the scan succeeds" +
                  (r.has_value() ? std::string{} : " (got: " + r.error().message + ")"));
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // ---- S4: the shared-digest escape hatch, at the default cap (where the old dry run's hard-coded
    //          64 could not hide the bug).
    {
        IdentityHandle scanner = authority.mint_root("precheck-s4-scanner");
        IdentityHandle sharer = authority.mint_root("precheck-s4-sharer");
        fs::path const root = fresh_dir("ae_precheck_s4");
        Ledger<> ledger;
        auto const contested = ledger.put_blob_safe(to_bytes("contested content"), sharer);
        saturate(ledger, "contested content", static_cast<int>(Ledger<>::kMaxAclRootsPerDigest) - 1, "precheck-s4");
        bool const saturated = !ledger.would_accept_blob_write(to_bytes("contested content"), scanner);
        bool const shared = contested.has_value() && ledger.mark_digest_shared(*contested, false, sharer).has_value();
        auto r = scan_two(ledger, root, scanner);
        check(saturated, "S4: setup -- the digest is at its cap of 64 before it is shared");
        check(shared && r.has_value() && r->entries.size() == 2,
              "S4: a digest at its cap and then marked shared -- the scan succeeds" +
                  (r.has_value() ? std::string{} : " (got: " + r.error().message + ")"));
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // ---- S5: the dry run predicts put_blob_safe(), step by step.
    {
        std::mt19937 rng(20260914);  // fixed: a failure must be reproducible
        std::vector<IdentityHandle> writers;
        for (int i = 0; i < 6; ++i) writers.push_back(authority.mint_root("precheck-s5-" + std::to_string(i)));
        std::vector<std::string> const contents = {"c0", "c1", "c2", "c3", "c4"};

        int disagreements = 0;
        int refusals = 0;
        int accepts = 0;
        std::string first;
        for (std::size_t const cap : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
                                      Ledger<>::kMaxAclRootsPerDigest}) {
            Ledger<> ledger(InMemoryWorktreeObjectStore{}, std::nullopt, cap);
            for (int step = 0; step < 800; ++step) {
                IdentityHandle const& w = writers[std::uniform_int_distribution<std::size_t>(0, writers.size() - 1)(rng)];
                std::string const& c = contents[std::uniform_int_distribution<std::size_t>(0, contents.size() - 1)(rng)];
                if (std::uniform_int_distribution<int>(0, 49)(rng) == 0) {
                    // Occasionally share: only succeeds for an already-authorized writer, which is the point.
                    auto const d = compute_digest(to_bytes(c));
                    if (d.has_value()) (void)ledger.mark_digest_shared(*d, false, w);
                }
                bool const predicted = ledger.would_accept_blob_write(to_bytes(c), w);
                bool const actual = ledger.put_blob_safe(to_bytes(c), w).has_value();
                (actual ? accepts : refusals) += 1;
                if (predicted != actual && disagreements++ == 0) {
                    first = "cap " + std::to_string(cap) + " step " + std::to_string(step) + ": predicted " +
                            (predicted ? "accept" : "refuse") + ", put " + (actual ? "accepted" : "refused");
                }
            }
        }
        check(refusals > 400 && accepts > 400,
              "S5: the sequences exercise both outcomes (" + std::to_string(accepts) + " accepted, " +
                  std::to_string(refusals) + " refused)");
        check(disagreements == 0,
              "S5 (the guard): would_accept_blob_write() predicted every one of 4000 put_blob_safe() outcomes" +
                  (disagreements == 0 ? std::string{}
                                      : " -- " + std::to_string(disagreements) + " disagreements, first " + first));
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
