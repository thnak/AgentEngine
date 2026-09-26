// Positive-control test for docs/planning/office-document-extraction-design-draft.md's "Prove pass 2" --
// the two central safety claims that draft's ten design revisions and nine red-team rounds ultimately
// rest on, argued from reading AppContainerProfile::grant_path()'s real code (Prove pass 1, "Red-team
// round 6"/"round 8") but never, until this file, proven by actually running anything.
//
// Claim 1 (foundation of the Revision 8 pool-elimination collapse, "Fix for finding 14"): an
// AppContainerProfile::grant_path() ACE lives on the granted path's OWN NTFS security descriptor, not on
// any profile-wide ledger -- deleting the object destroys the ACE as a structural side effect, and a
// FRESH object later created at the exact same path carries no trace of it (nothing "leaks" back in).
// This also proves the underlying claim finding 33 needed to even be found: a directory's (OI)(CI) grant
// materializes an INDEPENDENT copy of the ACE on every child created inside it while the grant is live --
// not a live reference back to the parent -- which is exactly why Claim 2 below needs a per-child revoke.
//
// Claim 2 (Revision 9/10's "Fix for finding 31"/"finding 33" safety-net fallback): a NEW function,
// AppContainerProfile::revoke_path() -- built by this same pass, mirroring grant_path()'s own three-call
// shape with SetEntriesInAclW's mode flipped to REVOKE_ACCESS -- actually strips this profile's SID's
// ACE from a directory AND from each of its independently-materialized children individually, and is a
// safe no-op (not a failure) against a child that was never created, exactly as the design's "fixed,
// four-path enumeration" fallback (finding 33) requires to be callable unconditionally.
//
// Claim 3 (added in "Prove pass 3", closing Prove pass 2's disclosed residual, finding 34): revoke_path()
// still succeeds -- on both the directory and a child -- while a REAL, live handle is held open on that
// exact child without FILE_SHARE_DELETE, the precise contention that makes remove_all() itself fail with
// ERROR_SHARING_VIOLATION. This is the scenario the design's cleanup-failure fallback (finding 31) exists
// for; finding 34 argued WRITE_DAC/READ_CONTROL isn't routed through NTFS's sharing-violation check the
// way DELETE/data opens are, but never ran it. This claim reproduces the remove_all() failure for real
// (a sanity check inside the test itself, not assumed) before proving revoke_path() succeeds anyway.
//
// Technique: the same dacl_entry_count()/GetNamedSecurityInfoW positive-control discipline
// test_native_jail_grant_ro_path_once_windows.cpp and test_native_jail_teardown_cycles_windows.cpp
// already established and validated as sensitive (a differing-value repeat grant genuinely accumulates
// ACEs there), extended here with a SID-specific has_sid_ace() check -- a flat entry count alone cannot
// distinguish "our ACE is gone" from "some OTHER ACE was added or removed," which matters once a
// directory carries more than one ACE (its own default/inherited entries plus our grant).

#include <windows.h>

#include <aclapi.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "backends/native_jail/app_container_profile.hpp"
#include "support/error_detail.hpp"

using namespace agentengine;
using agentengine::native_jail::AppContainerProfile;

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

// Same technique as test_native_jail_grant_ro_path_once_windows.cpp's own dacl_entry_count().
DWORD dacl_entry_count(std::wstring const& path) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS || dacl == nullptr) return 0;
    ACL_SIZE_INFORMATION size_info{};
    GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation);
    if (sd != nullptr) LocalFree(sd);
    return size_info.AceCount;
}

// A flat count cannot tell us WHICH entry changed. This walks the real DACL and counts how many
// ACCESS_ALLOWED_ACEs belong to `target_sid` specifically -- the actual claim both grant_path() and
// revoke_path() make promises about. A path that no longer exists has no ACE for anyone, by definition.
// (Windows legitimately splits ONE inheritable-generic-rights grant_path() call into TWO ACEs for the
// same SID -- an effective ACE on the object itself plus an inherit-only ACE for future children, real
// ACL canonicalization, not a bug -- so this counts rather than assumes exactly one.)
DWORD count_sid_aces(std::wstring const& path, PSID target_sid) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS || dacl == nullptr) return 0;

    DWORD count = 0;
    ACL_SIZE_INFORMATION size_info{};
    GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation);
    for (DWORD i = 0; i < size_info.AceCount; ++i) {
        LPVOID ace = nullptr;
        if (!GetAce(dacl, i, &ace)) continue;
        auto const* header = static_cast<ACE_HEADER const*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;  // the only type grant_path() ever sets
        auto const* allowed = static_cast<ACCESS_ALLOWED_ACE const*>(ace);
        PSID const ace_sid = const_cast<PSID>(reinterpret_cast<void const*>(&allowed->SidStart));
        if (EqualSid(ace_sid, target_sid)) ++count;
    }
    if (sd != nullptr) LocalFree(sd);
    return count;
}

bool has_sid_ace(std::wstring const& path, PSID target_sid) { return count_sid_aces(path, target_sid) > 0; }

void write_small_file(std::filesystem::path const& p, char const* content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}

}  // namespace

int main() {
    auto profile_result =
        AppContainerProfile::ensure(L"AgentEngine.NativeJail", L"AgentEngine Native Jail",
                                     L"AgentEngine native-jail sandbox AppContainer profile (008 SS1b, ADR-004)");
    if (!profile_result.has_value()) {
        std::cerr << "AppContainerProfile::ensure() failed: "
                   << agentengine::test_support::describe(profile_result.error()) << "\n";
        return 1;
    }
    AppContainerProfile const profile = std::move(*profile_result);
    PSID const sid = profile.sid();

    // ==== Claim 1: grant_path() + real deletion -- the ACE does not outlive the object it lives on ====
    {
        std::filesystem::path const dir =
            std::filesystem::temp_directory_path() / "ae_prove_pass2_claim1";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        AE_CHECK(!ec, "Claim 1 setup: scratch directory exists");

        std::wstring const dir_w = dir.wstring();
        AE_CHECK(!has_sid_ace(dir_w, sid), "Claim 1: a freshly created directory carries no ACE for our SID yet");

        auto granted = profile.grant_path(dir_w, /*read_write=*/true);
        if (!granted.has_value()) {
            std::cerr << "grant_path() error: " << agentengine::test_support::describe(granted.error())
                       << "\n";
        }
        AE_CHECK(granted.has_value(), "Claim 1: grant_path() succeeds on a real, existing directory");
        AE_CHECK(has_sid_ace(dir_w, sid), "Claim 1: the directory's own DACL now carries our SID's ACE");

        // A file created AFTER the grant, while it is live, should independently materialize a copy of
        // the (OI)(CI) ACE -- this is the exact mechanism finding 33 found: an inheritance COPY, not a
        // live reference. Proving it here grounds Claim 2's whole premise, not just Claim 1's.
        std::filesystem::path const child = dir / "child.txt";
        write_small_file(child, "prove pass 2 claim 1 child");
        std::wstring const child_w = child.wstring();
        AE_CHECK(has_sid_ace(child_w, sid),
                 "Claim 1 (finding 33's own premise): a file created inside the granted directory "
                 "independently carries its own copy of the SID's ACE via (OI)(CI) inheritance");

        auto const removed = std::filesystem::remove_all(dir, ec);
        AE_CHECK(!ec && removed != static_cast<std::uintmax_t>(-1),
                 "Claim 1: remove_all() on the granted, populated directory succeeds");
        AE_CHECK(!std::filesystem::exists(dir, ec), "Claim 1: the directory no longer exists after remove_all()");

        // Re-create a FRESH object at the exact same path, WITHOUT any new grant call. If the ACE had
        // somehow survived at the OS/profile level rather than living purely on the deleted object, it
        // would reappear here. It does not -- proving the ACE genuinely did not outlive the object.
        std::filesystem::create_directories(dir, ec);
        AE_CHECK(!ec, "Claim 1: a fresh object can be created at the same path afterward");
        AE_CHECK(!has_sid_ace(dir_w, sid),
                 "Claim 1 (the central claim): a FRESH object at the same path, with no new grant call, "
                 "carries no trace of the deleted object's ACE -- deletion genuinely destroyed it, "
                 "nothing survived at any profile-wide or OS-wide level");

        std::filesystem::remove_all(dir, ec);
    }

    // ==== Claim 2: revoke_path() strips a directory's ACE AND each independently-materialized child's,
    // and is a safe no-op against a child that was never created (finding 33's fixed enumeration) ====
    {
        std::filesystem::path const dir =
            std::filesystem::temp_directory_path() / "ae_prove_pass2_claim2";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        AE_CHECK(!ec, "Claim 2 setup: scratch directory exists");

        std::wstring const dir_w = dir.wstring();
        auto granted = profile.grant_path(dir_w, /*read_write=*/true);
        AE_CHECK(granted.has_value(), "Claim 2 setup: grant_path() succeeds on the scratch directory");

        // The design's own fixed, three-child enumeration (finding 33/"Sourcing"): args.json, source,
        // out.json. Deliberately omit a fourth candidate name to exercise the "never created" branch.
        std::filesystem::path const args_json = dir / "args.json";
        std::filesystem::path const source = dir / "source";
        std::filesystem::path const out_json = dir / "out.json";
        std::filesystem::path const never_created = dir / "out.json.does_not_exist_for_this_test";
        write_small_file(args_json, "{}");
        write_small_file(source, "prove pass 2 claim 2 source stand-in");
        write_small_file(out_json, "{\"result\": \"ok\"}");

        std::wstring const args_w = args_json.wstring();
        std::wstring const source_w = source.wstring();
        std::wstring const out_w = out_json.wstring();
        std::wstring const never_created_w = never_created.wstring();

        AE_CHECK(has_sid_ace(dir_w, sid), "Claim 2: directory carries our SID's ACE before revoke");
        AE_CHECK(has_sid_ace(args_w, sid),
                 "Claim 2: args.json independently carries its own materialized copy of the ACE");
        AE_CHECK(has_sid_ace(source_w, sid),
                 "Claim 2: the source-document stand-in independently carries its own materialized ACE");
        AE_CHECK(has_sid_ace(out_w, sid),
                 "Claim 2: out.json independently carries its own materialized ACE");

        DWORD const dir_total_before = dacl_entry_count(dir_w);
        DWORD const dir_sid_aces_before = count_sid_aces(dir_w, sid);
        AE_CHECK(dir_sid_aces_before >= 1,
                 "Claim 2 setup: the directory carries at least one ACE for our SID before revoke");

        // The fixed, four-path enumeration finding 33's fallback specifies: the directory plus its
        // (up to) three known children -- including one that was never created, proving that branch is
        // a safe no-op, not a failure, exactly as the design requires for an unconditional call.
        auto revoke_dir = profile.revoke_path(dir_w);
        auto revoke_args = profile.revoke_path(args_w);
        auto revoke_source = profile.revoke_path(source_w);
        auto revoke_out = profile.revoke_path(out_w);
        auto revoke_missing = profile.revoke_path(never_created_w);

        if (!revoke_dir.has_value())
            std::cerr << "revoke_path(dir) error: " << test_support::describe(revoke_dir.error()) << "\n";
        if (!revoke_args.has_value())
            std::cerr << "revoke_path(args.json) error: " << test_support::describe(revoke_args.error())
                       << "\n";
        if (!revoke_source.has_value())
            std::cerr << "revoke_path(source) error: " << test_support::describe(revoke_source.error())
                       << "\n";
        if (!revoke_out.has_value())
            std::cerr << "revoke_path(out.json) error: " << test_support::describe(revoke_out.error())
                       << "\n";
        if (!revoke_missing.has_value())
            std::cerr << "revoke_path(never_created) error: "
                       << test_support::describe(revoke_missing.error()) << "\n";

        AE_CHECK(revoke_dir.has_value(), "Claim 2: revoke_path(dir) succeeds");
        AE_CHECK(revoke_args.has_value(), "Claim 2: revoke_path(args.json) succeeds");
        AE_CHECK(revoke_source.has_value(), "Claim 2: revoke_path(source) succeeds");
        AE_CHECK(revoke_out.has_value(), "Claim 2: revoke_path(out.json) succeeds");
        AE_CHECK(revoke_missing.has_value(),
                 "Claim 2 (finding 33's own required branch): revoke_path() against a child that was "
                 "never created is a safe no-op, not a failure -- ERROR_FILE_NOT_FOUND treated as "
                 "trivially satisfied");

        AE_CHECK(!has_sid_ace(dir_w, sid), "Claim 2: the directory no longer carries our SID's ACE after revoke");
        AE_CHECK(!has_sid_ace(args_w, sid),
                 "Claim 2 (the central claim finding 33 found missing): args.json's OWN independently-"
                 "materialized ACE is gone -- revoking the parent's ACE alone would NOT have achieved "
                 "this, proving the per-child enumeration is load-bearing, not redundant");
        AE_CHECK(!has_sid_ace(source_w, sid),
                 "Claim 2: the source-document stand-in's own independently-materialized ACE is gone");
        AE_CHECK(!has_sid_ace(out_w, sid), "Claim 2: out.json's own independently-materialized ACE is gone");

        // Revoke is surgical -- it removes every ACE for OUR SID specifically (Windows legitimately
        // splits one inheritable grant_path() call into more than one ACE for the same SID -- an
        // effective ACE plus an inherit-only ACE for future children, real canonicalization, not a
        // bug, so this does not assume a fixed count) and touches NOTHING else: every other SID's own
        // ACE count on this directory is unchanged, proving it is not a side effect of clearing the
        // whole DACL.
        DWORD const dir_total_after = dacl_entry_count(dir_w);
        DWORD const dir_sid_aces_after = count_sid_aces(dir_w, sid);
        AE_CHECK(dir_sid_aces_after == 0,
                 "Claim 2: zero ACEs for our SID remain on the directory after revoke_path()");
        AE_CHECK(dir_total_after - dir_sid_aces_after == dir_total_before - dir_sid_aces_before,
                 "Claim 2: revoke_path() removes ONLY our SID's own ACE(s) -- every other SID's ACE "
                 "count on the directory is unchanged, a real surgical revoke, not a side effect of "
                 "clearing the whole DACL");

        std::filesystem::remove_all(dir, ec);
    }

    // ==== Claim 3 (Prove pass 2's disclosed residual, finding 34): revoke_path() succeeds under real
    // open-handle contention that causes remove_all() itself to fail -- the exact scenario finding 31's
    // safety-net fallback exists to handle, argued from documented WRITE_DAC/READ_CONTROL semantics in
    // finding 34 but never executed until this pass. ====
    {
        std::filesystem::path const dir =
            std::filesystem::temp_directory_path() / "ae_prove_pass3_claim3";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        AE_CHECK(!ec, "Claim 3 setup: scratch directory exists");

        std::wstring const dir_w = dir.wstring();
        auto granted = profile.grant_path(dir_w, /*read_write=*/true);
        AE_CHECK(granted.has_value(), "Claim 3 setup: grant_path() succeeds on the scratch directory");

        std::filesystem::path const held = dir / "held_open.txt";
        write_small_file(held, "prove pass 3 claim 3 held-open child");
        std::wstring const held_w = held.wstring();
        AE_CHECK(has_sid_ace(held_w, sid),
                 "Claim 3 setup: the held-open child independently carries its own materialized ACE");

        // Open WITHOUT FILE_SHARE_DELETE -- any later delete attempt on this exact path, by us or by
        // remove_all()'s own implementation, gets ERROR_SHARING_VIOLATION for as long as this handle
        // stays open. Real contention, not simulated.
        HANDLE const held_handle =
            CreateFileW(held_w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        AE_CHECK(held_handle != INVALID_HANDLE_VALUE,
                 "Claim 3 setup: obtained a real handle to the child file, opened without FILE_SHARE_DELETE");

        // Sanity check: confirm remove_all() genuinely fails while the handle is held, rather than
        // assuming it. If this doesn't reproduce a real failure, the rest of this claim proves nothing.
        std::error_code remove_ec;
        auto const removed = std::filesystem::remove_all(dir, remove_ec);
        AE_CHECK(remove_ec || removed == static_cast<std::uintmax_t>(-1),
                 "Claim 3 (sanity check): remove_all() on the directory genuinely FAILS while the child "
                 "is held open without FILE_SHARE_DELETE -- the real, reproduced failure mode "
                 "revoke_path() exists to have a fallback for, not an assumed one");
        AE_CHECK(std::filesystem::exists(dir, ec) && std::filesystem::exists(held, ec),
                 "Claim 3: the directory and held-open file both still exist -- remove_all() genuinely "
                 "did not complete");

        // While the handle is STILL open -- the exact live-contention window finding 34 argued about --
        // call revoke_path() on the directory and the held-open child, per the design's fixed-enumeration
        // fallback (finding 33).
        auto revoke_dir = profile.revoke_path(dir_w);
        auto revoke_held = profile.revoke_path(held_w);
        if (!revoke_dir.has_value())
            std::cerr << "revoke_path(dir) error: " << test_support::describe(revoke_dir.error()) << "\n";
        if (!revoke_held.has_value())
            std::cerr << "revoke_path(held) error: " << test_support::describe(revoke_held.error()) << "\n";

        AE_CHECK(revoke_dir.has_value(),
                 "Claim 3 (the central claim): revoke_path(dir) succeeds even though the directory is "
                 "not empty and a child inside it is held open with a delete-blocking share mode");
        AE_CHECK(revoke_held.has_value(),
                 "Claim 3 (the central claim): revoke_path(held-open child) succeeds DESPITE the handle "
                 "still being open on that exact file -- WRITE_DAC/READ_CONTROL genuinely is not routed "
                 "through NTFS's sharing-violation check the way DELETE/data opens are");

        AE_CHECK(!has_sid_ace(dir_w, sid),
                 "Claim 3: the directory's ACE is gone despite the failed remove_all() and the still-open "
                 "handle");
        AE_CHECK(!has_sid_ace(held_w, sid),
                 "Claim 3: the held-open child's own independently-materialized ACE is gone too, while "
                 "the handle is STILL open -- revoke_path() succeeded exactly where remove_all() failed, "
                 "the real evidence finding 34's reasoning was argued but never run for");

        CloseHandle(held_handle);

        // Now that the handle is closed, cleanup can finally succeed -- confirms the earlier failure was
        // genuinely caused by the held handle, not some other problem with this directory.
        std::error_code cleanup_ec;
        std::filesystem::remove_all(dir, cleanup_ec);
        AE_CHECK(!cleanup_ec, "Claim 3: after closing the handle, remove_all() succeeds -- confirming the "
                              "earlier failure really was caused by the open handle, not something else");
    }

    if (g_failures == 0) {
        std::cout << "test_native_jail_grant_path_ace_lifecycle_windows: all checks passed\n";
        return 0;
    }
    std::cerr << "test_native_jail_grant_path_ace_lifecycle_windows: " << g_failures << " check(s) failed\n";
    return 1;
}
