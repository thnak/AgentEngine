// Milestone 3 Phase E5 / decisions/ADR-015-shellrunner-grammar-parser-fuzzing.md -- 010-Python-Code-
// Interpreter.md §9 G8's libFuzzer harness against `agentengine::native_jail::mediated_shell::parse`
// (mediated_shell_parser.hpp), the genuinely new E3 parser (untouched spike:
// shell_parser.{hpp,cpp}, ADR-001's own already-fuzzed prove-phase evidence, stays out of this).
//
// `parse` is a pure `bytes -> result<ScriptNode>` function BY TYPE (mediated_shell_parser.hpp's own
// header comment): no FileSystemAdapter, no CommandRegistry, no ExecState, no EffectContext
// reachable from inside it -- exactly the shape a libFuzzer target needs (no fakes, no I/O, no
// global mutable state to reset between iterations; the arena is fully owned by the returned
// ParsedScript and destructs at the end of each call).
//
// Only built when AGENTENGINE_BUILD_SHELL_FUZZER is ON (CMakeLists.txt), which itself requires a
// real Clang front end -- libFuzzer's runtime (-fsanitize=fuzzer) has no MSVC-native equivalent, the
// same documented toolchain gap this repo already carries for UBSan (ADR-001/005/006/007/009).

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "backends/native_jail/mediated_shell_parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size) {
    std::string_view source(reinterpret_cast<char const*>(data), size);
    auto result = agentengine::native_jail::mediated_shell::parse(source);
    // The return value is deliberately unused beyond letting `result`'s destructor run (which tears
    // down the arena on a successful parse) -- this harness's whole claim is "no input, valid or
    // malformed, crashes/hangs/reads out of bounds," not anything about which inputs are accepted.
    (void)result;
    return 0;
}
