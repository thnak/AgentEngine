// Proof for `include/agentengine/pal/console.hpp` (GitHub issue #48): the four interactive CLIs
// under tools/ printed a real model's UTF-8 bytes through a Windows console still set to its OEM
// code page, so one em dash came out as three CP437 glyphs.
//
// The mojibake happens inside the console host, and this test's own stdout under ctest is a pipe,
// where bytes pass through untranscoded and the bug cannot appear at all. So the symptom is observed
// where it actually occurs: R1/R2 write the bytes to the console and read back out of its SCREEN
// BUFFER, as UTF-16, the characters the host decoded them into. The remaining checks assert the
// state the console is left in, which is the whole of what this scope does.
//
//   R1 (reproduces the bug) -- on CP437 the console decodes the three UTF-8 bytes of one em dash
//         into three wrong characters: exactly the glyphs issue #48 saw in a real model's reply.
//   R2 (fixes it) -- inside the scope, the same bytes decode into the single character U+2014.
//
//   I1 (control) -- a character injected into the console input buffer reads back as its byte, so
//         I2-I4 measure something real rather than always reporting an empty read.
//   I2 (reproduces the input-side bug) -- at CP437 a typed e-acute arrives as one byte no UTF-8
//         reader can decode, which is what goes out on the wire to a provider today.
//   I3 -- inside the scope the same keystroke arrives as valid UTF-8.
//   I4 -- ASCII input still reads back correctly at CP 65001. Console UTF-8 INPUT was genuinely
//         broken on Windows before 10 1903, which is why some code sets only the output page; this
//         checks the claim on the machine in front of it instead of inheriting it.
//
//   W1 (positive control) -- the console is first forced to CP437 and read back, so a non-UTF-8
//         state is demonstrably observable here. Without this, W2 is a check that cannot fail on a
//         machine whose console happened to be UTF-8 already.
//   W2 -- inside the scope both the output AND the input code page are 65001, and the scope reports
//         what it is holding.
//   W3 -- leaving the scope puts both back. The code page belongs to the console the parent shell is
//         still using; changing it permanently is a side effect on somebody else's window.
//   W4 -- a console already on UTF-8 is left alone and reports nothing held, so a later restore
//         cannot "put back" 65001 over a value the scope never set.
//   W5 -- `restore()` does the same job explicitly and is idempotent, and after it the destructor
//         does NOT clobber a code page set by someone else afterwards. This is the path
//         `tools/cli_chat.cpp` depends on: it ends its success path with `std::_Exit(0)`, which runs
//         no destructor and no atexit handler, so a destructor-only design would silently restore
//         nothing there.
//   W6 -- with no console attached at all, the scope changes nothing, reports nothing, and does not
//         fail. A CLI whose output is redirected to a file must still start.
//   P1 -- off Windows the whole type is a no-op that still compiles, constructs, restores and
//         reports honestly. There is nothing to fix: a POSIX terminal decodes by locale.
//
// Needs no daemon, no network and no privileges. W6 is skipped, with the reason printed, when this
// test's own stdout IS the console -- freeing it would take the test's output with it.

#include "agentengine/pal/console.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks  = 0;
int g_failed  = 0;
int g_skipped = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

// A skip is printed with its reason and counted, so a run that quietly exercised nothing cannot look
// like a clean pass.
[[maybe_unused]] void skip(std::string const& what, std::string const& why) {
    ++g_skipped;
    std::printf("[skip] %s -- %s\n", what.c_str(), why.c_str());
}

#if defined(_WIN32)
// Writes `bytes` to the console and reads back the characters the CONSOLE HOST actually decoded them
// into, straight out of its screen buffer as UTF-16. This is the one way to observe the reported
// symptom from inside a test: the mojibake is produced by the host, and this test's own stdout under
// ctest is a pipe, where bytes are passed through untouched and the bug cannot appear at all.
//
// Empty on any failure, which the caller reports as a skip rather than a pass.
[[nodiscard]] std::wstring render_through_console(std::string const& bytes) {
    HANDLE const con = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                     0, nullptr);
    if (con == INVALID_HANDLE_VALUE) return {};

    std::wstring                out;
    CONSOLE_SCREEN_BUFFER_INFO  before{};
    if (::GetConsoleScreenBufferInfo(con, &before) != 0) {
        // Column 0 of the current row, so a short write can never wrap and the cells read back are
        // exactly the cells just written.
        COORD const start{0, before.dwCursorPosition.Y};
        if (::SetConsoleCursorPosition(con, start) != 0) {
            DWORD written = 0;
            if (::WriteFile(con, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) !=
                0) {
                CONSOLE_SCREEN_BUFFER_INFO after{};
                if (::GetConsoleScreenBufferInfo(con, &after) != 0 &&
                    after.dwCursorPosition.Y == start.Y && after.dwCursorPosition.X > 0) {
                    out.resize(static_cast<std::size_t>(after.dwCursorPosition.X));
                    DWORD read = 0;
                    if (::ReadConsoleOutputCharacterW(con, out.data(),
                                                      static_cast<DWORD>(out.size()), start,
                                                      &read) != 0) {
                        out.resize(read);
                    } else {
                        out.clear();
                    }
                }
            }
            // Leave the cursor where it was found, so a scratch console does not scroll away.
            ::SetConsoleCursorPosition(con, start);
        }
    }
    ::CloseHandle(con);
    return out;
}

// Injects `typed` into the console's input buffer as real key events and returns the BYTES a caller
// like `std::getline(std::cin, ...)` would then read -- which the host encodes through the separate
// INPUT code page. This is how the input half of the fix is checked without a human at the keyboard.
//
// Bounded by a real timeout on every wait, so a console that delivers nothing fails the check rather
// than hanging CI. Empty on any failure, which the caller reports as a skip.
[[nodiscard]] std::string type_through_console(std::wstring const& typed, std::size_t want_bytes) {
    HANDLE const in = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                    nullptr);
    if (in == INVALID_HANDLE_VALUE) return {};

    // Raw mode: no line editing and no echo, so a read returns exactly the injected characters
    // without waiting for an Enter this test cannot press.
    DWORD saved_mode = 0;
    bool const had_mode = ::GetConsoleMode(in, &saved_mode) != 0;
    if (!had_mode || ::SetConsoleMode(in, 0) == 0) {
        ::CloseHandle(in);
        return {};
    }
    ::FlushConsoleInputBuffer(in);

    std::vector<INPUT_RECORD> keys;
    keys.reserve(typed.size());
    for (wchar_t const c : typed) {
        INPUT_RECORD rec{};
        rec.EventType                        = KEY_EVENT;
        rec.Event.KeyEvent.bKeyDown          = TRUE;
        rec.Event.KeyEvent.wRepeatCount      = 1;
        rec.Event.KeyEvent.uChar.UnicodeChar = c;
        keys.push_back(rec);
    }

    std::string out;
    DWORD       injected = 0;
    if (::WriteConsoleInputW(in, keys.data(), static_cast<DWORD>(keys.size()), &injected) != 0 &&
        injected == keys.size()) {
        // At most a handful of short reads, each with its own wait, until the expected bytes arrive.
        for (int attempt = 0; attempt < 8 && out.size() < want_bytes; ++attempt) {
            if (::WaitForSingleObject(in, 500) != WAIT_OBJECT_0) break;
            char  buf[64] = {};
            DWORD got     = 0;
            if (::ReadFile(in, buf, static_cast<DWORD>(sizeof buf), &got, nullptr) == 0) break;
            out.append(buf, got);
        }
    }

    ::SetConsoleMode(in, saved_mode);
    ::CloseHandle(in);
    return out;
}
#endif

}  // namespace

int main() {
    using agentengine::pal::ConsoleUtf8Scope;

#if !defined(_WIN32)
    {
        ConsoleUtf8Scope scope;
        check(!scope.output_changed() && !scope.input_changed(),
              "P1: off Windows the scope changes nothing");
        check(scope.previous_output_cp() == 0 && scope.previous_input_cp() == 0,
              "P1: ... and reports nothing to put back");
        scope.restore();
        check(!scope.output_changed() && !scope.input_changed(),
              "P1: restore() is a no-op that leaves the same honest report");
    }
#else
    // This process may legitimately have no console: ctest gives it pipes, and a CI runner may have
    // no attached console at all. Borrow one for the duration if so.
    bool allocated_console = false;
    if (::GetConsoleOutputCP() == 0 && ::AllocConsole() != 0) allocated_console = true;

    UINT const original_out = ::GetConsoleOutputCP();
    UINT const original_in  = ::GetConsoleCP();

    if (original_out == 0) {
        skip("R1-W5", "this process has no console and AllocConsole() failed");
        // The no-console path is exactly what this machine can prove, so prove it here instead of
        // reporting nothing at all.
        ConsoleUtf8Scope scope;
        check(!scope.output_changed() && !scope.input_changed(),
              "W6: with no console at all, the scope changes nothing and does not fail");
        check(scope.previous_output_cp() == 0 && scope.previous_input_cp() == 0,
              "W6: ... and reports nothing to put back");
    } else {
        // ---- R1/R2: the reported symptom itself -- reproduced, then fixed -- read back out of the
        // console host's own screen buffer rather than inferred from the code-page register.
        std::string const em_dash_utf8 = "\xE2\x80\x94";  // U+2014, the character issue #48 found
        std::wstring const as_oem =
                ::SetConsoleOutputCP(437) != 0 ? render_through_console(em_dash_utf8) : std::wstring{};
        if (as_oem.empty()) {
            skip("R1/R2", "this console's screen buffer could not be written and read back");
        } else {
            // CP437: 0xE2 is Γ, 0x80 is Ç, 0x94 is ö -- byte for byte, the three glyphs the issue
            // reported seeing in a real model's reply.
            check(as_oem == std::wstring{L'\x0393', L'\x00C7', L'\x00F6'},
                  "R1 (reproduces the bug): on CP437 the console decodes one em dash into three "
                  "wrong characters");
            std::wstring as_utf8;
            {
                ConsoleUtf8Scope scope;
                as_utf8 = render_through_console(em_dash_utf8);
            }
            check(as_utf8 == std::wstring{L'\x2014'},
                  "R2 (fixes it): inside the scope the same bytes decode into the single character "
                  "U+2014");
        }

        // ---- I1-I4: the input half, typed for real into the console's own input buffer. This is
        // the half that carries actual risk: console UTF-8 INPUT was genuinely broken on Windows
        // builds before 10 1903, so I4 checks that plain ASCII still reads back at CP 65001 rather
        // than taking it on faith.
        ::SetConsoleCP(437);
        std::string const ascii_at_oem = type_through_console(L"A", 1);
        if (ascii_at_oem.empty()) {
            skip("I1-I4", "this console's input buffer could not be written and read back");
        } else {
            check(ascii_at_oem == "A",
                  "I1 (control): a character injected into the console reads back as its byte, so "
                  "I2-I4 are measuring something real");
            // CP437 encodes U+00E9 as the single byte 0x82. UTF-8 encodes it as C3 A9.
            check(type_through_console(L"\x00E9", 1) == std::string("\x82", 1),
                  "I2 (the input-side bug): at CP437 a typed e-acute reaches the program as one "
                  "byte no UTF-8 reader can decode");
            std::string utf8_typed;
            std::string ascii_typed;
            {
                ConsoleUtf8Scope scope;
                utf8_typed  = type_through_console(L"\x00E9", 2);
                ascii_typed = type_through_console(L"hi", 2);
            }
            check(utf8_typed == "\xC3\xA9",
                  "I3: inside the scope the same keystroke arrives as valid UTF-8, which is what "
                  "goes out on the wire to the provider");
            check(ascii_typed == "hi",
                  "I4: ASCII input still reads back correctly at CP 65001 -- the residual that "
                  "makes some code set only the output page");
        }
        ::SetConsoleCP(original_in);

        // ---- W1: the positive control. CP437 is the OEM US page, built into every Windows; if the
        // console cannot be moved off UTF-8 here then W2 below proves nothing.
        bool const forced = ::SetConsoleOutputCP(437) != 0 && ::SetConsoleCP(437) != 0;
        check(forced, "W1 (positive control): the console can be put on a non-UTF-8 code page");
        check(forced && ::GetConsoleOutputCP() == 437 && ::GetConsoleCP() == 437,
              "W1 (positive control): CP437 reads back, so W2's assertion is one that can fail");

        // ---- W2/W3: the fix itself, and putting the console back afterwards.
        {
            ConsoleUtf8Scope scope;
            check(::GetConsoleOutputCP() == CP_UTF8,
                  "W2: the output code page is UTF-8 inside the scope -- this is the bug's fix");
            check(::GetConsoleCP() == CP_UTF8,
                  "W2: the input code page is UTF-8 too, so a non-ASCII prompt reaches the model "
                  "as the user typed it");
            check(scope.output_changed() && scope.input_changed(),
                  "W2: the scope reports holding both pages");
            check(scope.previous_output_cp() == 437 && scope.previous_input_cp() == 437,
                  "W2: ... and remembers exactly what to put back");
        }
        check(::GetConsoleOutputCP() == 437,
              "W3: the previous output code page is restored when the scope ends");
        check(::GetConsoleCP() == 437,
              "W3: the previous input code page is restored when the scope ends");

        // ---- W4: a console already on UTF-8 must be left alone, not "restored" to 65001 later by a
        // scope that never changed it.
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);
        {
            ConsoleUtf8Scope scope;
            check(!scope.output_changed() && !scope.input_changed(),
                  "W4: a console already on UTF-8 is reported as unchanged, so nothing is held");
            check(::GetConsoleOutputCP() == CP_UTF8, "W4: ... and it is still UTF-8 inside the scope");
        }
        check(::GetConsoleOutputCP() == CP_UTF8 && ::GetConsoleCP() == CP_UTF8,
              "W4: ... and still UTF-8 after it, untouched");

        // ---- W5: the explicit path, which is the only one cli_chat.cpp's std::_Exit(0) can use.
        ::SetConsoleOutputCP(437);
        ::SetConsoleCP(437);
        {
            ConsoleUtf8Scope scope;
            check(::GetConsoleOutputCP() == CP_UTF8, "W5 (precondition): the scope took effect");
            scope.restore();
            check(::GetConsoleOutputCP() == 437 && ::GetConsoleCP() == 437,
                  "W5: an explicit restore() puts both pages back without unwinding the scope");
            check(!scope.output_changed() && !scope.input_changed(),
                  "W5: ... and the scope then reports holding nothing");
            scope.restore();
            check(::GetConsoleOutputCP() == 437, "W5: restore() is idempotent");
            // Somebody else changes the console after this scope gave it up. The destructor about to
            // run must not overwrite that.
            ::SetConsoleOutputCP(CP_UTF8);
        }
        check(::GetConsoleOutputCP() == CP_UTF8,
              "W5: after restore(), the destructor does not clobber a later change");

        // Every path from here on leaves the console alone, so put back what this test borrowed --
        // unconditionally, before the branch below can take an exit that skips it. Without this, the
        // skip path left the developer's own shell on 65001 for good: precisely the side effect the
        // scope under test exists to avoid, reintroduced by its own test.
        ::SetConsoleOutputCP(original_out);
        ::SetConsoleCP(original_in);

        // ---- W6: no console at all. Done last, because it gives this console up for good.
        DWORD  console_mode = 0;
        HANDLE const out    = ::GetStdHandle(STD_OUTPUT_HANDLE);
        bool const stdout_is_console =
                out != INVALID_HANDLE_VALUE && ::GetConsoleMode(out, &console_mode) != 0;
        if (stdout_is_console) {
            skip("W6", "this test's own stdout IS the console; freeing it would take the output too");
        } else {
            // The code pages were already put back just above -- which had to happen BEFORE this
            // point, since after FreeConsole this process can no longer reach that console at all
            // while the parent shell still can.
            if (::FreeConsole() == 0) {
                skip("W6", "FreeConsole() failed");
            } else {
                allocated_console = false;  // gone; nothing left to release below
                ConsoleUtf8Scope scope;
                check(!scope.output_changed() && !scope.input_changed(),
                      "W6: with no console at all, the scope changes nothing and does not fail");
                check(scope.previous_output_cp() == 0 && scope.previous_input_cp() == 0,
                      "W6: ... and reports nothing to put back");
            }
        }
    }

    if (allocated_console) {
        ::SetConsoleOutputCP(original_out);
        ::SetConsoleCP(original_in);
        ::FreeConsole();
    }
#endif

    // A run that asserted nothing is a failure, not a pass -- the shape this project has caught
    // before, where every branch skipped and the summary still said ALL PASS.
    if (g_checks == 0) {
        ++g_failed;
        std::printf("[FAIL] no check ran at all\n");
    }

    std::printf("\n%d checks, %d failed, %d skipped\n", g_checks, g_failed, g_skipped);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
