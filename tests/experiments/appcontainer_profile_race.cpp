// Does CreateAppContainerProfile race when several processes create the SAME profile name at once?
//
// NativeJailBackend uses one machine-global AppContainer profile name for the whole project --
// L"AgentEngine.NativeJail" (native_jail_backend.cpp, shared_profile_state()) -- and `ctest -j 4`
// runs six Windows native-jail test binaries concurrently, each a separate process calling
// CreateAppContainerProfile with that same name. The profile lives at
// %LOCALAPPDATA%\Packages\<name> plus registry state, so "several processes create it at once" is a
// real question and not a hypothetical one.
//
// It matters because of an intermittent CI and local failure (~1 run in 40-80) where every create()
// in one test process fails while a concurrent one fails differently:
//
//     abuse_corpus_windows : CreateAppContainerProfile failed: HRESULT 0x8007000A
//                            = HRESULT_FROM_WIN32(ERROR_BAD_ENVIRONMENT), "the environment is incorrect"
//     backend_windows      : CreateProcessW failed: Win32 error 2  (ERROR_FILE_NOT_FOUND)
//
// NativeJailBackend caches that profile error in a magic static, so ONE transient failure poisons
// every later create() in that process -- which is why the symptom is "all nine create()s failed"
// rather than "one did".
//
// Build (MSVC developer shell):
//     cl /nologo /O2 /EHsc /std:c++20 tests/experiments/appcontainer_profile_race.cpp ^
//        /Fe:acrace.exe /link userenv.lib
//
// Run N copies concurrently against one name -- one process alone proves nothing here:
//     1..8 | ForEach-Object { Start-Process .\acrace.exe -ArgumentList "AgentEngine.RaceProbe",200 }
//
// Exit code is the number of UNEXPECTED failures (ERROR_ALREADY_EXISTS is expected and is what the
// product treats as success).
//
// MEASURED 2026-08-16, MSVC 14.44, Windows 11 -- the answer is yes, and it is not marginal:
//
//   1 process  x  50 iterations, no lock : 0 unexpected
//   8 processes x 300 iterations, no lock : 149-158 unexpected PER PROCESS (~50%), worst 0x80070005
//   8 processes x 300 iterations, "lock" : 0 unexpected, all 2400 calls  <-- the mitigation
//
// The third row is the same binary under the same contention with only the named mutex added, which
// is what makes it a control rather than an absence of evidence. DeriveAppContainerSidFromApp-
// ContainerName measured 0 failures in every configuration and needs no protection.
//
// The mitigation now lives in the product: app_container_profile.cpp's CrossProcessLock, holding the
// same mutex name across the create call only.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <userenv.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")  // FreeSid

int wmain(int argc, wchar_t** argv) {
    std::wstring name = argc > 1 ? argv[1] : L"AgentEngine.RaceProbe";
    int iters = argc > 2 ? _wtoi(argv[2]) : 200;
    // Third argument "lock" reproduces the mitigation shipped in app_container_profile.cpp
    // (CrossProcessLock): a named mutex held across the create call only. Running the probe both
    // ways is the point -- "0 failures with the lock" means nothing unless the same binary,
    // same contention, produces failures without it.
    bool use_lock = (argc > 3 && std::wstring(argv[3]) == L"lock");
    std::wstring const mutex_name = L"AgentEngine.AppContainerProfileCreate." + name;

    int unexpected = 0, already = 0, ok = 0, derive_failed = 0;
    HRESULT worst = S_OK;

    for (int i = 0; i < iters; ++i) {
        PSID created = nullptr;
        HANDLE lock = nullptr;
        bool held = false;
        if (use_lock) {
            lock = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
            if (lock != nullptr) {
                DWORD rc = WaitForSingleObject(lock, 30'000);
                held = (rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED);
            }
        }
        HRESULT hr = CreateAppContainerProfile(name.c_str(), L"race probe", L"race probe", nullptr,
                                                0, &created);
        if (lock != nullptr) {
            if (held) ReleaseMutex(lock);
            CloseHandle(lock);
        }
        if (SUCCEEDED(hr)) {
            ++ok;
        } else if (HRESULT_CODE(hr) == ERROR_ALREADY_EXISTS) {
            ++already;
        } else {
            // The interesting bucket: neither created nor already-there. This is what the product
            // turns into app_container.create_profile_failed.
            ++unexpected;
            worst = hr;
        }
        if (created != nullptr) FreeSid(created);

        // The product always derives the SID afterwards rather than trusting `created`, so race the
        // same call the same way.
        PSID derived = nullptr;
        HRESULT dhr = DeriveAppContainerSidFromAppContainerName(name.c_str(), &derived);
        if (FAILED(dhr)) {
            ++derive_failed;
            worst = dhr;
        }
        if (derived != nullptr) FreeSid(derived);
    }

    printf("pid=%lu  lock=%s  created=%d already_exists=%d UNEXPECTED=%d derive_failed=%d  worst=0x%08lX\n",
           GetCurrentProcessId(), use_lock ? "yes" : "NO ", ok, already, unexpected, derive_failed,
           static_cast<unsigned long>(worst));
    return unexpected + derive_failed;
}
