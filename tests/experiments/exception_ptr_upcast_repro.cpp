// The decisive reproducer for docs/research/2026-08-16-clang-windows-asan-exception-ptr.md,
// deliberately minimal.
//
// NO coroutine and NO AgentEngine headers. The research note identifies the shape shared by all
// three CI findings as: capture an exception into std::exception_ptr, rethrow it, catch it by
// reference to a DERIVED type, then call what() -- which is a derived->base upcast UBSan checks for
// alignment, and a dereference ASan can fault on.
//
// If this reproduces under clang on Windows, the findings belong to the toolchain and no AgentEngine
// code is implicated. If it does NOT reproduce, the finding is specific to something this project
// does and the competing reading (a real defect visible only on Windows) gains real weight.
//
// Exit 0 = clean. A UBSan report or an ASan fault is the interesting result; -fno-sanitize-recover
// makes UBSan abort, so a nonzero exit is a finding.

#include <cstdio>
#include <exception>
#include <stdexcept>

int main() {
    std::exception_ptr captured;
    try {
        throw std::runtime_error("boom");
    } catch (...) {
        captured = std::current_exception();  // the round-trip the CI findings all go through
    }

    try {
        std::rethrow_exception(captured);
    } catch (std::runtime_error const& e) {
        // The upcast under test: `e` is std::runtime_error&, what() is std::exception::what().
        std::printf("what=%s\n", e.what());
    }

    std::printf("repro: clean\n");
    return 0;
}
