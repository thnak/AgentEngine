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
// idiom scattered through tests/ called it twice in one expression for that reason.
//
// Empty-vs-absent, stated accurately because an earlier version of this comment got it wrong: on
// POSIX the two are distinct and this function reports a set-but-empty variable as an engaged
// optional holding "". On WINDOWS they are not distinguishable at all -- `_putenv("X=")` DELETES the
// variable, so a set-but-empty value cannot exist and both `std::getenv` and this function report
// absent. Do not build behaviour on the distinction; a caller that wants them treated alike anyway
// can say `.value_or("")`.
//
// NOT for credentials. Reading a secret through this leaves an un-wiped plaintext heap copy; use
// `env_var_consume` below, which exists for exactly that reason.

#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
    // `buf` is freed on EVERY exit path, including the two that are easy to miss: a non-zero return
    // (documented to leave buf null, but not relied on) and a throwing std::string construction.
    struct BufGuard {
        char* p;
        ~BufGuard() {
            if (p != nullptr) std::free(p);
        }
    };
    int const rc = ::_dupenv_s(&buf, &len, name);
    BufGuard guard{buf};
    if (rc != 0) return std::nullopt;
    if (buf == nullptr) return std::nullopt;
    return std::string(buf, len == 0 ? 0 : len - 1);  // len counts the NUL
#else
    char const* v = std::getenv(name);
    if (v == nullptr) return std::nullopt;
    return std::string(v);
#endif
}

[[nodiscard]] inline std::optional<std::string> env_var(std::string const& name) {
    return env_var(name.c_str());
}

namespace env_detail {
// Best-effort zeroization the optimizer may not elide, touching each byte through a volatile
// pointer. Deliberately a duplicate of `trust/secret.hpp`'s `secret_detail::secure_zero` rather than
// a shared include: the PAL is the bottom layer and must not depend on `trust/`, and this is four
// lines of primitive, not a design. Keep the two in step.
inline void secure_zero(void* p, std::size_t n) noexcept {
    auto* v = static_cast<volatile unsigned char*>(p);
    while (n-- > 0) *v++ = 0;
}
}  // namespace env_detail

// The secret-bearing read. `fn` is invoked with a view of the value and MUST consume it before
// returning; the view is not valid afterwards. Returns false iff the variable is not set (in which
// case `fn` is not called).
//
// This exists because `env_var` above is the WRONG function for a credential. It returns an owned
// `std::string`, so reading a secret through it leaves a plaintext heap copy that is freed without
// being wiped -- two of them on MSVC, counting `_dupenv_s`'s own buffer -- directly against
// `trust/secret.hpp`'s stated contract ("`Secret` ZEROIZES its buffer on destruction ... there is
// intentionally NO `std::string` conversion"). Migrating `EnvSecretSource` onto `env_var` for
// warning-cleanliness quietly weakened exactly the property that module exists to provide.
//
// Here, POSIX makes no copy at all -- `fn` sees the environment block directly, which is what the
// pre-PAL code did -- and MSVC wipes `_dupenv_s`'s buffer before freeing it. Neither path leaves a
// plaintext copy behind.
template <class Fn>
[[nodiscard]] inline bool env_var_consume(char const* name, Fn&& fn) {
    if (name == nullptr) return false;
#if defined(_MSC_VER)
    char*       buf = nullptr;
    std::size_t len = 0;
    if (::_dupenv_s(&buf, &len, name) != 0) return false;
    if (buf == nullptr) return false;
    std::size_t const chars = (len == 0) ? 0 : len - 1;  // len counts the NUL
    try {
        std::forward<Fn>(fn)(std::string_view(buf, chars));
    } catch (...) {
        env_detail::secure_zero(buf, len);  // never leave plaintext behind on the throwing path
        std::free(buf);
        throw;
    }
    env_detail::secure_zero(buf, len);
    std::free(buf);
    return true;
#else
    char const* v = std::getenv(name);
    if (v == nullptr) return false;
    // No copy: `fn` reads the environment block itself, exactly as the pre-PAL code did.
    std::forward<Fn>(fn)(std::string_view(v));
    return true;
#endif
}

template <class Fn>
[[nodiscard]] inline bool env_var_consume(std::string const& name, Fn&& fn) {
    return env_var_consume(name.c_str(), std::forward<Fn>(fn));
}

}  // namespace agentengine::pal
