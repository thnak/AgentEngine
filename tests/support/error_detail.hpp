#pragma once
// Render an `ae::error` in full when a test reports a failure.
//
// This exists because of a concrete diagnostic dead end. `test_native_jail_backend_windows` failed
// in CI (run 31939239439, Windows / MSVC / ASan) with:
//
//     FAIL: C2: create() succeeds given a real mount and ResourceLimits (handle.has_value())
//     create() failed, aborting remaining checks
//
// and that is the whole record. `NativeJailBackend::create()` has five distinct failure returns --
// the shared AppContainer profile's init_error, a BlobRef mount, grant_path's three separate Win32
// paths, and JobObjectLimits::create -- each carrying a distinct `code` and often a `native_code`
// straight from GetLastError(). The test discarded all of it and printed a sentence that does not
// distinguish any of them, so an intermittent failure on a runner nobody can attach a debugger to
// is unactionable: you cannot tell a racing profile creation from a denied ACL edit from a job
// limit rejection.
//
// `error` already carries everything needed (message, code, native_code -- core/error.hpp); nothing
// had to be added to the product to make this diagnosable, it only had to be printed.
#include <expected>
#include <iostream>
#include <string>

#include "agentengine/core/error.hpp"

namespace agentengine::test_support {

inline std::string describe(ae::error const& e) {
    std::string s = "code=" + e.code + " class=" + std::to_string(static_cast<int>(e.klass));
    if (e.native_code != 0) s += " native_code=" + std::to_string(e.native_code);
    s += " message=\"" + e.message + "\"";
    return s;
}

// Convenience for the common `if (!r.has_value())` shape.
template <class T>
inline std::string describe_error(std::expected<T, ae::error> const& r) {
    return r.has_value() ? std::string("<no error>") : describe(r.error());
}

}  // namespace agentengine::test_support

// Assert `res` holds a value, and on failure print WHICH error it holds. Expands the including
// file's own AE_CHECK -- every test in this suite defines that macro identically, and expansion
// happens at the use site, so the definition order does not matter.
//
// Use this instead of a bare `AE_CHECK(res.has_value(), ...)` anywhere the expected value comes from
// a fallible platform call. The bare form is what turned an intermittent AppContainer failure into
// nine identical "create() succeeds" FAIL lines with no code attached to any of them.
#define AE_CHECK_OK(res, label)                                                                  \
    do {                                                                                          \
        AE_CHECK((res).has_value(), label);                                                       \
        if (!(res).has_value()) {                                                                 \
            std::cerr << "    -> " << ::agentengine::test_support::describe((res).error())        \
                      << "\n";                                                                    \
        }                                                                                         \
    } while (0)
