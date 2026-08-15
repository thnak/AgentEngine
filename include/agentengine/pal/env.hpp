#pragma once
// One portable environment-variable read, so that no call site has to choose between a warning and a
// suppression.
//
// `std::getenv` is the standard, portable spelling and it is what this codebase wants to express.
// MSVC nonetheless deprecates it (C4996) in favour of the non-standard `_dupenv_s`, and the two
// other required compilers (g++ 14+, clang 20+) have no equivalent complaint. Before this header the
// codebase resolved that tension the wrong way, at `trust/secret.hpp`: a local
// `#pragma warning(disable : 4996)` around the call. A pragma is still a suppression -- it makes the
// analyzer stop reporting rather than making the reported thing go away -- and this project does not
// allow that: a warning is fixed at its site or it goes through red-team into an ADR.
//
// So the platform difference is handled where platform differences belong, in the PAL, exactly once,
// using each platform's own non-deprecated API: `_dupenv_s` on MSVC, `std::getenv` everywhere else.
// The result is that /W4 and -Wall -Wextra are both clean with nothing turned off.
//
// The returned `std::optional<std::string>` is a COPY, which is also a correctness improvement over
// the raw `char const*` it replaces: the pointer `std::getenv` hands back is only valid until the
// next `getenv`/`putenv` call, and the previous `std::getenv("TEMP") ? std::getenv("TEMP") : "..."`
// idiom scattered through tests/ called it twice in one expression for that reason. An empty-string
// value is reported as a present, empty string (`std::optional` holding ""), never folded into
// "absent" -- POSIX and Windows both distinguish the two, and a caller that wants them treated alike
// can say `.value_or("")` or check `->empty()` itself.

#include <optional>
#include <string>

#if defined(_MSC_VER)
#include <cstdlib>  // _dupenv_s, free
#else
#include <cstdlib>  // std::getenv
#endif

namespace agentengine::pal {

// Reads `name` from the process environment. `std::nullopt` when the variable is not set.
[[nodiscard]] inline std::optional<std::string> env_var(char const* name) {
    if (name == nullptr) return std::nullopt;
#if defined(_MSC_VER)
    // _dupenv_s allocates; the buffer is ours to free. It returns 0 and leaves `buf == nullptr` when
    // the variable simply is not set, which is not an error -- only a non-zero return is.
    char*       buf = nullptr;
    std::size_t len = 0;
    if (::_dupenv_s(&buf, &len, name) != 0) return std::nullopt;
    if (buf == nullptr) return std::nullopt;
    std::optional<std::string> out(std::string(buf, len == 0 ? 0 : len - 1));  // len counts the NUL
    std::free(buf);
    return out;
#else
    char const* v = std::getenv(name);
    if (v == nullptr) return std::nullopt;
    return std::string(v);
#endif
}

[[nodiscard]] inline std::optional<std::string> env_var(std::string const& name) {
    return env_var(name.c_str());
}

}  // namespace agentengine::pal
