#pragma once
// One portable "make this console speak UTF-8" scope, so that a real model's own words survive the
// last few inches of the trip to a Windows terminal (GitHub issue #48).
//
// THE BUG THIS EXISTS FOR. The engine is UTF-8 end to end -- `Message`/`ContentItem` text, tool
// arguments, skill bodies and every provider's wire format are UTF-8 bytes -- and the interactive
// drivers under `tools/` hand those bytes straight to `std::cout`. The Windows console host does not
// read them as UTF-8. It decodes whatever it is given through the console's OUTPUT code page, which
// on an out-of-the-box install is the OEM page (437 on an English-locale machine; 850, 866, 932 and
// others elsewhere). So one em dash in a model's reply -- U+2014, bytes `E2 80 94` -- arrives as the
// three CP437 glyphs the issue reproduced live against a real model. Every curly quote, accented
// letter and non-Latin script in this project's own demonstration surface has the same fate, on the
// four CLIs that exist specifically to show a session working.
//
// POSIX needs no equivalent step: a terminal there decodes by the user's locale, which is UTF-8 on
// every platform this project builds for. So this whole type is an empty, trivially-destructible
// no-op off Windows rather than a second mechanism to keep in sync.
//
// WHY A SCOPE RATHER THAN A BARE CALL AT THE TOP OF `main`. A code page belongs to the CONSOLE, not
// to the process -- the same console the parent `cmd.exe` or PowerShell is still using. Setting it
// and walking away leaves that window at 65001 for every later command the user types, which is a
// real, visible side effect on somebody else's shell (and a genuine one: legacy console tools that
// still emit ANSI-code-page bytes then mojibake in the other direction). The previous values are
// therefore captured and put back.
//
// ... EXCEPT WHEN THE PROCESS REFUSES TO UNWIND. `tools/cli_chat.cpp` deliberately ends its success
// path with `std::_Exit(0)`, to dodge a real CPython finalize-thread crash; that skips destructors
// AND `atexit` handlers, so a destructor-only design would silently never restore anything there --
// the exact class of "looks correct, does nothing" this codebase keeps finding. `restore()` is
// therefore public and idempotent, and that CLI calls it explicitly on the line before its `_Exit`.
//
// BEST EFFORT, NEVER FATAL. A process can legitimately have no console at all: output redirected to
// a file, launched from a service, or a GUI-subsystem parent. Then `GetConsoleOutputCP()` reports 0
// and `SetConsoleOutputCP()` fails, and nothing is wrong -- bytes written to a pipe or a file are
// never transcoded, so there was nothing to fix. Every entry point here is `noexcept`, returns no
// error, and reports what it actually changed through `output_changed()`/`input_changed()` so a test
// can tell "did not need to" apart from "tried and failed". Nothing in this header can make a CLI
// fail to start.
//
// CONSTRUCT IT BEFORE THE FIRST WRITE. The console consults the code page when bytes reach it, not
// when they enter `std::cout`'s buffer, so a mid-stream change would decode already-buffered text
// under the new page. Declaring this as the first statement in `main` makes the question moot, and
// is the only supported use.
//
// INPUT TOO, AND THE ONE RESIDUAL. These CLIs read the user's prompt with `std::getline(std::cin,
// ...)`, and the console encodes that line through the separate INPUT code page. Left at OEM, a
// non-English user's own typing reaches the model as mojibake -- the same bug mirrored, and worse,
// because invalid UTF-8 then goes out on the wire to a provider. So both pages are set.
//
// The known objection to setting the input page is that console UTF-8 *input* was genuinely broken
// on Windows builds before 10 1903 (`ReadFile` on a console handle at CP 65001 could return zero
// bytes), which is why some older code sets only the output page. That is not inherited on faith
// here: `tests/rt/test_console_utf8.cpp` I1-I4 inject real key events into the console's own input
// buffer and read the bytes back, and I4 checks ASCII specifically, on whatever host the test runs
// on. If a build where it does fail ever matters, split the two flags rather than dropping both.
//
// WHAT THIS DOES NOT FIX, so that a box on screen is not read as a failure of this code. The code
// page decides how bytes become characters; the console FONT decides whether a character has a
// glyph. A terminal still on a raster font shows U+2014 as a box or a blank -- correctly decoded and
// still unreadable. That is a font setting on the user's terminal, not something a process should
// change on a console it is only borrowing.
//
// ONE RESIDUAL, NAMED. A process that dies without unwinding -- a crash, a `TerminateProcess`, a
// Ctrl+Break -- leaves the console at 65001, because there is no exit path left to put it back.
// That is a cosmetic effect on the surviving shell, not a correctness one (UTF-8 is the better
// default there anyway), and closing it would mean a console control handler in a PAL header, which
// is a larger mechanism than the bug justifies.

// No project includes on purpose: this is a leaf PAL header with no error type of its own (see
// "best effort, never fatal" above), so it stays as cheap to include as the one system header it
// needs on Windows and nothing at all elsewhere.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace agentengine::pal {

// Puts this process's console into UTF-8 for its lifetime, and puts it back afterwards.
//
// Non-copyable and non-movable on purpose: two live scopes would each believe they own the restore,
// and the second one's "previous" value would be 65001, so unwinding would leave the console at
// UTF-8 forever. One per process, at the top of `main`.
class ConsoleUtf8Scope {
public:
    ConsoleUtf8Scope() noexcept {
#if defined(_WIN32)
        // 0 means "no console attached" -- not an error, just nothing to do.
        UINT const out_cp = ::GetConsoleOutputCP();
        if (out_cp != 0 && out_cp != CP_UTF8 && ::SetConsoleOutputCP(CP_UTF8) != 0) {
            previous_output_cp_ = out_cp;
            output_changed_     = true;
        }
        UINT const in_cp = ::GetConsoleCP();
        if (in_cp != 0 && in_cp != CP_UTF8 && ::SetConsoleCP(CP_UTF8) != 0) {
            previous_input_cp_ = in_cp;
            input_changed_     = true;
        }
#endif
    }

    ~ConsoleUtf8Scope() { restore(); }

    ConsoleUtf8Scope(ConsoleUtf8Scope const&)                = delete;
    ConsoleUtf8Scope& operator=(ConsoleUtf8Scope const&)     = delete;
    ConsoleUtf8Scope(ConsoleUtf8Scope&&) noexcept            = delete;
    ConsoleUtf8Scope& operator=(ConsoleUtf8Scope&&) noexcept = delete;

    // Puts back exactly what this scope changed, and nothing it did not. Idempotent -- safe to call
    // by hand before a `std::_Exit`, after which the destructor (if it ever runs) does nothing.
    void restore() noexcept {
#if defined(_WIN32)
        if (output_changed_) {
            ::SetConsoleOutputCP(previous_output_cp_);
            output_changed_ = false;
        }
        if (input_changed_) {
            ::SetConsoleCP(previous_input_cp_);
            input_changed_ = false;
        }
#endif
    }

    // True only while this scope is actually holding a changed code page: false when there was no
    // console, when the console was already UTF-8, when the change failed, and after `restore()`.
    [[nodiscard]] bool output_changed() const noexcept { return output_changed_; }
    [[nodiscard]] bool input_changed() const noexcept { return input_changed_; }

    // What will be put back, or 0 when this scope changed nothing.
    [[nodiscard]] unsigned previous_output_cp() const noexcept {
        return output_changed_ ? previous_output_cp_ : 0u;
    }
    [[nodiscard]] unsigned previous_input_cp() const noexcept {
        return input_changed_ ? previous_input_cp_ : 0u;
    }

private:
    bool output_changed_ = false;
    bool input_changed_  = false;
#if defined(_WIN32)
    UINT previous_output_cp_ = 0;
    UINT previous_input_cp_  = 0;
#else
    unsigned previous_output_cp_ = 0;
    unsigned previous_input_cp_  = 0;
#endif
};

}  // namespace agentengine::pal
